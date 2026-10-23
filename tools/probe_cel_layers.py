#!/usr/bin/env python
"""Read-only probe of the two CEL layers in the running Game.exe.

Walks app -> map -> map+0x34 / map+0x38, reads the player's cell from both
CEL buffers (plus the override buffers) and compares them with the file
layers.  No injection, no debugger, no writes.

    python tools/probe_cel_layers.py
    python tools/probe_cel_layers.py --cell 128,125

Current-SHA offsets (Game.exe 7a5e2c08…):
  app  = *(0x00ab91b0)
  map  = *(app + 0x904); map vtable 0x9809f8
  CEL1 = *(map + 0x34)  # first bitmap, param_5=1, normal ground layer
  CEL2 = *(map + 0x38)  # second bitmap, param_5=0, auxiliary layer
  player px = map+0xc / map+0x10; CEL cell = px >> 4
"""
from __future__ import annotations

import argparse
import ctypes
import os
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import kimi_probe_runtime as K  # noqa: E402
from dhsh.formats.map import parse_cel  # noqa: E402

APP_GLOBAL = 0x00AB91B0
MAP_PTR_OFF = 0x904
MAP_VTABLE = 0x9809F8
CEL_SLOTS = ((0x34, "ground", 1), (0x38, "aux", 0))
CEL_ROOTS = ("extracted/decoded", "extracted/decoded_0b", "extracted/easy")

k32 = ctypes.windll.kernel32
k32.ReadProcessMemory.restype = ctypes.c_bool
k32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


def read_n(h, addr: int, size: int, tries: int = 30):
    for _ in range(tries):
        buf = (ctypes.c_char * size)()
        got = ctypes.c_size_t()
        if k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size,
                                 ctypes.byref(got)) and got.value == size:
            return bytes(buf[:size])
        time.sleep(0.02)
    return None


def u32(h, addr: int):
    b = read_n(h, addr, 4)
    return struct.unpack("<I", b)[0] if b else None


def u8(h, addr: int):
    b = read_n(h, addr, 1)
    return b[0] if b else None


def read_cstr(h, addr: int, maxlen: int = 80):
    out = bytearray()
    for i in range(maxlen):
        b = read_n(h, addr + i, 1)
        if b is None:
            return None
        if b == b"\0":
            break
        out += b
    return bytes(out)


def find_cel_file(core_path: str):
    rel = core_path.replace("\\", "/")
    stem = os.path.splitext(rel)[0]
    for root in CEL_ROOTS:
        for ext in (".CEL", ".cel"):
            path = os.path.join(ROOT, root, stem + ext)
            if os.path.exists(path):
                return path
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cell", help="override CEL cell as X,Y")
    args = ap.parse_args()

    pid, h, base = K.find_game()
    if not h:
        print("Game.exe not found")
        return 2
    try:
        app = u32(h, base + (APP_GLOBAL - 0x400000))
        map_obj = u32(h, app + MAP_PTR_OFF) if app else None
        if not map_obj or u32(h, map_obj) != MAP_VTABLE:
            print(f"map object not found (app={app!r})")
            return 2
        px = u32(h, map_obj + 0xC)
        py = u32(h, map_obj + 0x10)
        if args.cell:
            cx, cy = (int(v) for v in args.cell.split(","))
        else:
            cx, cy = px >> 4, py >> 4
        print(f"pid={pid} map={map_obj:#x} app={app:#x}")
        print(f"player px=({px},{py}) cell=({cx},{cy})")

        container = u32(h, app + 0x88)
        core_path = None
        if container:
            raw = read_cstr(h, container + 0x44)
            if raw:
                core_path = raw.decode("gbk", "replace")
                print("container path:", core_path)
        cel_file = find_cel_file(core_path) if core_path else None
        file_layers = {}
        file_dims = None
        if cel_file:
            data = open(cel_file, "rb").read()
            f1 = parse_cel(data, 1)
            f0 = parse_cel(data, 0)
            file_dims = (f1.width, f1.height)
            for layer, f in ((1, f1), (0, f0)):
                file_layers[layer] = f.get(cx, cy)
            print("file:", os.path.relpath(cel_file, ROOT).replace(os.sep, "/"),
                  f"cells={f1.width}x{f1.height}",
                  "bits layer1/layer0 =", file_layers[1], file_layers[0])

        results = []
        for slot, label, layer in CEL_SLOTS:
            cel = u32(h, map_obj + slot)
            if not cel:
                print(f"{label}: CEL slot empty")
                continue
            w = u32(h, cel + 4)
            ht = u32(h, cel + 8)
            buf = u32(h, cel + 0xC)
            ov = u32(h, cel + 0x14)
            name = read_cstr(h, cel + 0x28) or b""
            idx = (cy + 1) * (w + 2) + 1 + cx
            b = u8(h, buf + idx) if buf else None
            o = u8(h, ov + idx) if ov else None
            results.append((label, layer, w, ht, b, o, name))
            print(f"{label}: map+{slot:#x} obj={cel:#x} w/h={w}x{ht} "
                  f"buf={buf:#x} ov={ov:#x} cell byte={b} override={o} "
                  f"name={name.decode('gbk', 'replace')}")
            if file_dims and (w, ht) != file_dims:
                if file_dims[0] <= w and file_dims[1] <= ht:
                    print(f"  padding: file {file_dims[0]}x{file_dims[1]} -> "
                          f"runtime {w}x{ht} (top-left; right/bottom blocked)")
                else:
                    print(f"  warning: file {file_dims[0]}x{file_dims[1]} "
                          f"larger than runtime {w}x{ht}")

        if results:
            ground = next((r for r in results if r[0] == "ground"), None)
            aux = next((r for r in results if r[0] == "aux"), None)
            if ground and ground[4] == 0:
                print("conclusion: ground layer is walkable at this cell "
                      "(first bitmap, 1=blocked)")
            if aux and aux[4] not in (None, 0):
                print("conclusion: auxiliary layer is blocked at this cell")
        return 0
    finally:
        k32.CloseHandle(h)


if __name__ == "__main__":
    sys.exit(main())
