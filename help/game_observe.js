'use strict';
// Frida read-only observer for the supplied x86 sample. No script execution or packet changes.
const MAX_BYTES = 256;
const STACK = true;
let seq = 0;
const sockets = new Map();
function log(kind, fields) {
  send(Object.assign({kind, seq: ++seq, time: new Date().toISOString(), tid: Process.getCurrentThreadId()}, fields));
}
function str(p) { try { return p.isNull() ? null : p.readUtf8String(512); } catch (_) { return '<unreadable>'; } }
function bytes(p, n) {
  try {
    const k = Math.min(Math.max(n, 0), MAX_BYTES);
    return Array.from(new Uint8Array(p.readByteArray(k))).map(x => x.toString(16).padStart(2, '0')).join('');
  } catch (_) { return '<unreadable>'; }
}
function trace(ctx) {
  if (!STACK) return [];
  try { return Thread.backtrace(ctx, Backtracer.ACCURATE).slice(0, 12).map(a => {
    const m = Process.findModuleByAddress(a);
    return m ? m.name + '+0x' + a.sub(m.base).toString(16) : a.toString();
  }); } catch (_) { return []; }
}
function checkedHook(mod, rva, expected, callbacks) {
  const p = mod.base.add(rva);
  try {
    const actual = Array.from(new Uint8Array(p.readByteArray(expected.length / 2))).map(x => x.toString(16).padStart(2, '0')).join('');
    if (actual !== expected) { log('hook_skipped', {address:p.toString(), rva, reason:'entry bytes differ', expected, actual}); return; }
    Interceptor.attach(p, callbacks);
    log('hook_installed', {address:p.toString(), rva});
  } catch (e) { log('hook_error', {rva, error:String(e)}); }
}
const game = Process.enumerateModules()[0];
log('startup', {module:game.name, base:game.base.toString(), arch:Process.arch, maxBytes:MAX_BYTES});
if (Process.arch === 'ia32' && game.name.toLowerCase() === 'game.exe') {
  checkedHook(game, 0x4ea880, '83ec088b4424108b5424188b4c241452', {
    onEnter(args) {
      this.record = {L:args[0].toString(), buffer:args[1].toString(), size:args[2].toUInt32(), chunk:str(args[3]), stack:trace(this.context)};
      this.record.prefixHex = bytes(args[1], this.record.size);
      log('lua_load_enter', this.record);
    },
    onLeave(ret) { log('lua_load_leave', {L:this.record.L, chunk:this.record.chunk, status:ret.toInt32()}); }
  });
  checkedHook(game, 0x4c6c80, '558be98b450485c0750633c05dc20c00', {
    onEnter(args) {
      this.obj = this.context.ecx.toString();
      // x86 __thiscall: ECX is this; args[0] is first stack argument.
      log('script_file_enter', {object:this.obj, filename:str(args[0]), stack:trace(this.context)});
    },
    onLeave(ret) { log('script_file_leave', {object:this.obj, result:ret.toInt32()}); }
  });
} else { log('lua_hooks_skipped', {reason:'expected x86 Game.exe'}); }
const hooked = new Set();
function hookNetwork(m) {
  if (m.name.toLowerCase() !== 'ws2_32.dll') return;
  for (const name of ['connect','send','recv','closesocket']) {
    if (hooked.has(name)) continue;
    const p = m.findExportByName(name);
    if (!p) continue;
    try {
      Interceptor.attach(p, {
        onEnter(args) {
          this.sock = args[0].toUInt32();
          this.call = ++seq;
          this.stack = trace(this.context);
          if (name === 'connect') {
            let peer = null;
            try {
              const a = args[1];
              if (args[2].toInt32() >= 16 && a.readU16() === 2) {
                const v = Array.from(new Uint8Array(a.readByteArray(8)));
                peer = v.slice(4,8).join('.') + ':' + ((v[2] << 8) | v[3]);
              }
            } catch (_) {}
            if (peer) sockets.set(this.sock, peer);
            this.peer = peer;
          }
          if (name === 'send' || name === 'recv') {
            this.buf = args[1]; this.length = args[2].toInt32();
            if (name === 'send') log('send_request', {call:this.call, socket:this.sock, peer:sockets.get(this.sock), buffer:this.buf.toString(), requested:this.length, prefixHex:bytes(this.buf,this.length), stack:this.stack});
          }
        },
        onLeave(ret) {
          const result = ret.toInt32();
          const data = {call:this.call, socket:this.sock, peer:this.peer || sockets.get(this.sock), result};
          if (name === 'recv') {
            data.requested = this.length;
            if (result > 0) {data.buffer=this.buf.toString(); data.prefixHex=bytes(this.buf,Math.min(result,this.length)); data.stack=this.stack;}
          }
          if (name === 'connect') data.stack = this.stack;
          log(name + '_result', data);
          if (name === 'closesocket' && result === 0) sockets.delete(this.sock);
        }
      });
      hooked.add(name); log('network_hook_installed', {name, address:p.toString()});
    } catch(e) { log('network_hook_error', {name, error:String(e)}); }
  }
}
// Covers a Winsock module already loaded or loaded after a suspended spawn.
const observer = Process.attachModuleObserver({onAdded:hookNetwork});
