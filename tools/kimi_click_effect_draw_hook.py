#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import frida

POINTS = {
    "view": (0x7F8600, "56 8b f1 e8 58 2e 00 00 84 c0 74 34 8b 44 24 0c"),
    "rect": (0x7FC161, "83 ff 06 75 37 8b 4e 04 0f bf 41 20 0f af 45 dc"),
}

JS = r"""
var specs = $SPECS;
function hexOf(p, n) {
  var b = new Uint8Array(p.readByteArray(n)), s = [];
  for (var i = 0; i < b.length; i++) s.push(('0' + b[i].toString(16)).slice(-2));
  return s.join(' ');
}
var verified = true;
Object.keys(specs).forEach(function (name) {
  var spec = specs[name];
  var actual = hexOf(ptr(spec[0]), 16);
  var ok = actual === spec[1];
  verified = verified && ok;
  send({ kind: 'verify', name: name, ok: ok, actual: actual });
});
if (!verified) {
  send({ kind: 'error', message: 'byte verification failed; hooks not installed' });
} else {
  var user32 = Process.getModuleByName('user32.dll');
  var getAsyncKeyState = new NativeFunction(user32.getExportByName('GetAsyncKeyState'), 'int16', ['int']);
  var getCursorPos = new NativeFunction(user32.getExportByName('GetCursorPos'), 'int', ['pointer']);
  var getForegroundWindow = new NativeFunction(user32.getExportByName('GetForegroundWindow'), 'pointer', []);
  var screenToClient = new NativeFunction(user32.getExportByName('ScreenToClient'), 'int', ['pointer', 'pointer']);
  var point = Memory.alloc(8);
  var wasDown = false;
  var busy = false;
  var stopped = false;
  var rectListener = null;
  var viewListener = null;
  var positions = {};

  function capture(clickMs, clickX, clickY, burst) {
    if (stopped) return;
    var remaining = 3000;
    var found = {};
    var rows = [];
    positions = {};
    viewListener = Interceptor.attach(ptr(specs.view[0]), {
      onEnter: function (args) {
        positions[args[2].toString()] = { x: args[0].toInt32(), y: args[1].toInt32() };
      }
    });
    rectListener = Interceptor.attach(ptr(specs.rect[0]), {
      onEnter: function () {
        if (--remaining === 0) {
          rectListener.detach();
          viewListener.detach();
          rectListener = null;
          viewListener = null;
          send({ kind: 'burst', clickMs: clickMs, clickX: clickX, clickY: clickY,
                 burst: burst, candidates: rows });
          if (burst < 5) {
            setTimeout(function () { capture(clickMs, clickX, clickY, burst + 1); }, 20);
          } else {
            busy = false;
          }
          return;
        }
        var w = this.context.ebp.sub(0x24).readS32();
        var h = this.context.ebp.sub(0x20).readS32();
        if (w < 8 || w > 96 || h < 4 || h > 64) return;
        var aspect = w / h;
        if (aspect < 0.8 || aspect > 6) return;
        var obj = this.context.esi;
        var vtable;
        try { vtable = obj.readPointer(); } catch (e) { return; }
        var key = obj.toString() + ':' + vtable.toString() + ':' + w + ':' + h;
        if (found[key]) return;
        found[key] = true;
        var pos = positions[obj.toString()];
        rows.push({ sinceClick: Date.now() - clickMs,
                    obj: obj.toString(), vtable: vtable.toString(), w: w, h: h,
                    x: pos ? pos.x : null, y: pos ? pos.y : null,
                    dx: pos ? pos.x - clickX : null, dy: pos ? pos.y - clickY : null,
                    mode: this.context.edi.toInt32() });
      }
    });
  }

  setInterval(function () {
    var down = (getAsyncKeyState(1) & 0x8000) !== 0;
    if (down && !wasDown && !busy) {
      var clickMs = Date.now();
      getCursorPos(point);
      var sx = point.readS32();
      var sy = point.add(4).readS32();
      screenToClient(getForegroundWindow(), point);
      var clickX = point.readS32();
      var clickY = point.add(4).readS32();
      busy = true;
      send({ kind: 'click', ms: clickMs, screenX: sx, screenY: sy, clientX: clickX, clientY: clickY });
      capture(clickMs, clickX, clickY, 0);
    }
    wasDown = down;
  }, 5);
  setTimeout(function () {
    stopped = true;
    busy = true;
    if (rectListener) rectListener.detach();
    if (viewListener) viewListener.detach();
    rectListener = null;
    viewListener = null;
    send({ kind: 'expired' });
  }, $DURATION_MS);
  send({ kind: 'ready' });
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--out", default=".rea/click_effect_draw.jsonl")
    args = parser.parse_args()
    pid = args.pid
    if pid is None:
        pid = next((p.pid for p in frida.get_local_device().enumerate_processes()
                    if p.name.lower() == "game.exe"), None)
    if pid is None:
        print("Game.exe is not running", flush=True)
        return 1

    output = Path(args.out)
    output.parent.mkdir(parents=True, exist_ok=True)
    session = frida.attach(pid)
    specs = {name: [hex(point), expected] for name, (point, expected) in POINTS.items()}
    script_source = JS.replace("$SPECS", json.dumps(specs)).replace(
        "$DURATION_MS", str(int(args.duration * 1000)))
    script = session.create_script(script_source)
    started = time.perf_counter()
    count = 0
    with output.open("w", encoding="utf-8") as fh:
        def on_message(message, data):
            nonlocal count
            if message.get("type") != "send":
                print("script:", message, flush=True)
                return
            payload = message["payload"]
            payload["t"] = round(time.perf_counter() - started, 3)
            fh.write(json.dumps(payload, ensure_ascii=False) + "\n")
            fh.flush()
            kind = payload["kind"]
            if kind == "verify":
                print(f"verify {payload['name']}: {'OK' if payload['ok'] else 'MISMATCH'}", flush=True)
            elif kind == "ready":
                print("ready: click walkable ground several times; keep the mouse still after each click", flush=True)
            elif kind == "click":
                print(f"[{payload['t']:7.3f}] CLICK", flush=True)
            elif kind == "burst":
                rows = payload["candidates"]
                count += len(rows)
                print(f"[{payload['t']:7.3f}] burst={payload['burst']} candidates={len(rows)}", flush=True)
                for row in rows:
                    print(f"  {row['w']}x{row['h']} xy=({row['x']},{row['y']}) "
                          f"sinceClick={row['sinceClick']} mode={row['mode']} "
                          f"obj={row['obj']} vt={row['vtable']}", flush=True)
            else:
                print(payload, flush=True)

        script.on("message", on_message)
        script.load()
        time.sleep(args.duration)

    script.unload()
    session.detach()
    print(f"done: {count} candidates; output={output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
