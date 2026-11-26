"""Read the live Hero2D client's NPC object table (read-only).

The 1.50.29 client creates NPC world objects with vtable 0x984554 (image base
0x400000).  Each object stores its display name inline at +4, its npc.pak
model/shape id at +0x28, its pixel centre at +0x2a/+0x2e and its current
direction at +0x32.  This tool scans private readable memory for that vtable
and prints/dumps the objects; it never writes to or attaches a debugger to the
target.

Usage:
  python tools/capture_npc_objects.py --pid 28816 --out manifests/batches/npc-objects.json
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import struct
import sys
from pathlib import Path

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
READABLE = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}

IMAGE_BASE = 0x400000
NPC_VTABLE_RVA = 0x584554  # 0x984554 - IMAGE_BASE
FIELD_NAME = 0x04
FIELD_SHAPE = 0x28
FIELD_X = 0x2A
FIELD_Y = 0x2E
FIELD_DIRECTION = 0x32

# YingTianFu.map WIDTH/HEIGHT (pixels); used only as a plausibility filter.
YINGTIANFU_WIDTH = 7808
YINGTIANFU_HEIGHT = 5760


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wt.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("th32ModuleID", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("GlblcntUsage", wt.DWORD),
        ("ProccntUsage", wt.DWORD),
        ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", wt.DWORD),
        ("hModule", wt.HMODULE),
        ("szModule", wt.CHAR * 256),
        ("szExePath", wt.CHAR * 260),
    ]


def module_base(k32, pid: int) -> tuple[int, str]:
    TH32CS_SNAPMODULE = 0x08
    TH32CS_SNAPMODULE32 = 0x10
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    entry = MODULEENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    base = 0
    path = ""
    if k32.Module32First(snap, ctypes.byref(entry)):
        while True:
            name = entry.szModule.decode("latin1", "replace")
            if name.lower() == "game.exe":
                base = int(entry.modBaseAddr or 0)
                path = entry.szExePath.decode("latin1", "replace")
                break
            if not k32.Module32Next(snap, ctypes.byref(entry)):
                break
    k32.CloseHandle(snap)
    return base, path


def decode_gbk(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("gbk", "replace")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--out", type=Path, default=Path("manifests/batches/npc-objects.json"))
    args = parser.parse_args()

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    k32.ReadProcessMemory.restype = wt.BOOL
    k32.ReadProcessMemory.argtypes = [
        wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    k32.VirtualQueryEx.restype = ctypes.c_size_t
    k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]

    process = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, args.pid)
    if not process:
        raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")
    base, exe_path = module_base(k32, args.pid)
    if not base:
        raise SystemExit("Game.exe module not found")
    vtable = base + NPC_VTABLE_RVA
    pattern = struct.pack("<I", vtable)

    def read(addr: int, size: int) -> bytes | None:
        buffer = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(
                process, ctypes.c_void_p(addr), buffer, size, ctypes.byref(got)):
            return None
        return buffer.raw[:got.value]

    chunk = 4 * 1024 * 1024
    buffer = ctypes.create_string_buffer(chunk)
    got = ctypes.c_size_t()
    found: dict[tuple[str, int, int, int], dict] = {}
    addr = 0
    while addr < 0x80000000:
        mbi = MBI()
        if not k32.VirtualQueryEx(
                process, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        region_base = int(mbi.BaseAddress or 0)
        region_size = int(mbi.RegionSize or 0x1000)
        next_addr = region_base + region_size
        readable = (
            mbi.State == MEM_COMMIT
            and not (mbi.Protect & PAGE_GUARD)
            and mbi.Protect != PAGE_NOACCESS
            and (mbi.Protect & 0xFF) in READABLE
        )
        if readable and mbi.Type == MEM_PRIVATE:
            pos = region_base
            while pos < region_base + region_size:
                size = min(chunk, region_base + region_size - pos)
                if k32.ReadProcessMemory(
                        process, ctypes.c_void_p(pos), buffer, size, ctypes.byref(got)):
                    data = buffer.raw[:got.value]
                    index = 0
                    while True:
                        hit = data.find(pattern, index)
                        if hit < 0:
                            break
                        index = hit + 4
                        obj_addr = pos + hit
                        object_bytes = read(obj_addr, 0x40)
                        if object_bytes is None or len(object_bytes) < 0x34:
                            continue
                        name = decode_gbk(object_bytes[FIELD_NAME:FIELD_NAME + 0x20])
                        shape = struct.unpack_from("<H", object_bytes, FIELD_SHAPE)[0]
                        x = struct.unpack_from("<H", object_bytes, FIELD_X)[0]
                        y = struct.unpack_from("<H", object_bytes, FIELD_Y)[0]
                        direction = object_bytes[FIELD_DIRECTION]
                        if not name or not (10000 <= shape <= 60001):
                            continue
                        if not (0 <= x <= YINGTIANFU_WIDTH and 0 <= y <= YINGTIANFU_HEIGHT):
                            continue
                        key = (name, shape, x, y)
                        found[key] = {
                            "name": name,
                            "shape": shape,
                            "x_px": x,
                            "y_px": y,
                            "cell_x": (x - 8) // 16,
                            "cell_y": (y - 8) // 16,
                            "direction": direction,
                        }
                pos += size
        if next_addr <= addr:
            break
        addr = next_addr

    k32.CloseHandle(process)
    objects = sorted(found.values(), key=lambda item: (item["name"], item["shape"]))
    exe_sha256 = ""
    if exe_path:
        try:
            exe_sha256 = hashlib.sha256(Path(exe_path).read_bytes()).hexdigest()
        except OSError:
            exe_sha256 = ""
    payload = {
        "source": {
            "process_id": args.pid,
            "game_exe": exe_path,
            "game_exe_sha256": exe_sha256,
            "module_base": f"0x{base:08x}",
            "npc_vtable": f"0x{vtable:08x}",
            "method": "read-only scan of private readable memory",
        },
        "objects": objects,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    for item in objects:
        print(f"{item['name']}\tshape={item['shape']}\tcell=({item['cell_x']},{item['cell_y']})"
              f"\tpx=({item['x_px']},{item['y_px']})\tdir={item['direction']}")
    print(f"{len(objects)} objects -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
