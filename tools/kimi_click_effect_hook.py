#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import frida

POINT = 0x8C40B0
EXPECTED = "8b 44 24 08 56 8b f1 8b 4c 24 08 50 51 8b ce e8 1c f6 ff ff 85 c0 74 1b 8b 46 2c 85 c0 7f 0b 6a 01 6a 00 8b ce e8 56 f5 ff ff b8 01 00 00 00 5e"

JS = r"""
var expected = $EXPECTED;
var point = ptr($POINT);
function hexOf(p, n) {
  var b = new Uint8Array(p.readByteArray(n)), s = [];
  for (var i = 0; i < b.length; i++) s.push(('0' + b[i].toString(16)).slice(-2));
  return s.join(' ');
}
var actual = hexOf(point, 48);
send({ kind: 'verify', point: point.toString(), expected: expected, actual: actual });
if (actual === expected) {
  Interceptor.attach(point, {
    onEnter: function (args) {
      var path = null;
      try { path = args[0].isNull() ? null : args[0].readCString(); } catch (e) { }
      send({ kind: 'load', path: path, flag: args[1].toInt32(),
             caller: this.returnAddress.toString(), thread: this.threadId });
    }
  });
  send({ kind: 'ready' });
} else {
  send({ kind: 'error', message: 'byte verification failed; hook not installed' });
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, default=17980)
    parser.add_argument("--duration", type=float, default=120.0)
    parser.add_argument("--out", default=".rea/click_effect_hook.jsonl")
    args = parser.parse_args()

    output = Path(args.out)
    output.parent.mkdir(parents=True, exist_ok=True)
    session = frida.attach(args.pid)
    script = session.create_script(
        JS.replace("$EXPECTED", json.dumps(EXPECTED)).replace("$POINT", hex(POINT))
    )
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
            if payload["kind"] == "verify":
                print("byte verification:", "OK" if payload["actual"] == EXPECTED else "MISMATCH", flush=True)
            elif payload["kind"] == "ready":
                print("ready: click walkable ground several times; do not move the mouse after pressing", flush=True)
            elif payload["kind"] == "load":
                count += 1
                print(f"[{payload['t']:7.3f}] path={payload['path']!r} flag={payload['flag']} caller={payload['caller']}", flush=True)
            else:
                print(payload, flush=True)

        script.on("message", on_message)
        script.load()
        time.sleep(args.duration)

    session.detach()
    print(f"done: {count} animation loads; output={output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
