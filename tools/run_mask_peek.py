#!/usr/bin/env python
"""One-shot runtime check for the MSK/core cover-mask container.

Run this AFTER launching the game and entering a map scene (not the login
screen).  It snapshots Game.exe, then reads the live container read-only:

    python tools/run_mask_peek.py

Checks:
  - app pointer = *(0x00ab91b0), container = *(app+0x88)
  - container +0x00 placement_count, +0x08 cap100, +0x0c cap50,
    +0x10 staging_count, +0x28/+0x2c vector, +0x40 disable flag
  - vector length == placement_count; flags/groups/f6 summary
  - key analysis bytes in the live module match the pinned snapshot

Writes manifests/batches/mask-peek-<sha>.json.  Exit 0 = all checks pass.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SNAPSHOT_TOOL = os.path.join(ROOT, "tools", "snapshot_gameexe.py")
DEFAULT_SOURCE = r"D:\software\dhsh\shdata\Game.exe"
SNAP_DIR = os.path.join(ROOT, "extracted", "gameexe-snapshots")
RECORD = os.path.join(ROOT, "manifests", "batches", "gameexe-snapshots.json")
OUT_DIR = os.path.join(ROOT, "manifests", "batches")

PROCESS_VM_READ = 0x10
PROCESS_QUERY_INFORMATION = 0x400
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10

GLOBAL_PTR = 0x00AB91B0
CONTAINER_OFF = 0x48 + 0x40
KEY_VAS = {
    "mask_parser": 0x4272D0,
    "mask_loader": 0x4278C0,
    "apply_mask": 0x4266D0,
    "depth_test": 0x426756,
    "group_frame": 0x426980,
    "mask_darken": 0x4264B0,
    "vtable_apply": 0x41E760,
}

k32 = ctypes.windll.kernel32
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p,
                                  ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.CloseHandle.argtypes = [wt.HANDLE]


class PROCESSENTRY32(ctypes.Structure):
    # WOW64 (32-bit) layout: pointer-sized fields are DWORDs.
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", wt.DWORD),
                ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", wt.LONG),
                ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_char * 260)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD),
                ("th32ProcessID", wt.DWORD), ("GlblcntUsage", wt.DWORD),
                ("ProccntUsage", wt.DWORD), ("modBaseAddr", wt.DWORD),
                ("modBaseSize", wt.DWORD), ("hModule", wt.DWORD),
                ("szModule", ctypes.c_char * 256),
                ("szExePath", ctypes.c_char * 260)]


k32.Process32First.restype = wt.BOOL
k32.Process32First.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
k32.Process32Next.restype = wt.BOOL
k32.Process32Next.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
k32.Module32First.restype = wt.BOOL
k32.Module32First.argtypes = [wt.HANDLE, ctypes.POINTER(MODULEENTRY32)]
k32.Module32Next.restype = wt.BOOL
k32.Module32Next.argtypes = [wt.HANDLE, ctypes.POINTER(MODULEENTRY32)]


def find_pid(name: str = "Game.exe") -> int | None:
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == ctypes.c_void_p(-1).value:
        return None
    e = PROCESSENTRY32()
    e.dwSize = ctypes.sizeof(PROCESSENTRY32)
    pid = None
    ok = k32.Process32First(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.decode(errors="ignore").lower() == name.lower():
            pid = e.th32ProcessID
            break
        ok = k32.Process32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def find_module_base(pid: int, name: str = "Game.exe") -> int | None:
    snap = k32.CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if not snap or snap == ctypes.c_void_p(-1).value:
        return None
    e = MODULEENTRY32()
    e.dwSize = ctypes.sizeof(MODULEENTRY32)
    base = None
    try:
        ok = k32.Module32First(snap, ctypes.byref(e))
        while ok:
            if e.szModule.decode(errors="ignore").lower() == name.lower():
                base = int(e.modBaseAddr or 0)
                break
            ok = k32.Module32Next(snap, ctypes.byref(e))
    except AttributeError:
        base = None
    k32.CloseHandle(snap)
    return base


class Reader:
    def __init__(self, pid: int):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                                 False, pid)
        if not self.h:
            raise OSError("OpenProcess failed, err=%d" % ctypes.GetLastError())

    def u32(self, addr: int) -> int:
        buf = ctypes.c_ulong(0)
        n = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr),
                                     ctypes.byref(buf), 4, ctypes.byref(n)) \
                or n.value != 4:
            raise OSError("RPM u32 @%08x failed" % addr)
        return buf.value

    def bytes(self, addr: int, size: int) -> bytes:
        buf = (ctypes.c_ubyte * size)()
        n = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size,
                                     ctypes.byref(n)) or n.value != size:
            raise OSError("RPM @%08x+%x failed" % (addr, size))
        return bytes(buf)

    def close(self) -> None:
        k32.CloseHandle(self.h)


def snapshot() -> tuple[str, str]:
    proc = subprocess.run([sys.executable, SNAPSHOT_TOOL],
                          capture_output=True, text=True)
    sha = "unknown"
    path = ""
    for line in proc.stdout.splitlines():
        if line.startswith("new snapshot:") or line.startswith("already recorded:"):
            sha = line.split(":", 1)[1].strip()
    rec = json.load(open(RECORD, encoding="utf-8"))
    for item in rec.get("snapshots", []):
        if item["sha256"] == sha:
            path = os.path.join(ROOT, item["snapshot"])
    print(proc.stdout.strip())
    return sha, path


def main() -> int:
    sha, snap_path = snapshot()
    pid = find_pid()
    if not pid:
        print("Game.exe not found.  Launch the game and enter a map scene first.")
        return 2
    base = find_module_base(pid) or 0x400000
    print(f"pid={pid} module_base={base:#x} sha={sha[:16]}")
    r = Reader(pid)
    result = {"sha256": sha, "pid": pid, "module_base": hex(base),
              "checks": {}, "elements": {}, "errors": []}
    try:
        app = r.u32(base + (GLOBAL_PTR - 0x400000))
        container = r.u32(app + CONTAINER_OFF) if app else 0
        result["app"] = hex(app)
        result["container"] = hex(container)
        print(f"app={app:#x} container={container:#x}")
        if not container:
            print("container NULL: game is probably not in a map scene.")
            result["errors"].append("container NULL (not in map scene)")
            return 2
        c = r.bytes(container, 0x60)
        f = lambda o: struct.unpack_from("<i", c, o)[0]
        u = lambda o: struct.unpack_from("<I", c, o)[0]
        placement_count = u(0)
        cap100, cap50, staging = f(8), f(0xC), f(0x10)
        vb, ve, vc = u(0x28), u(0x2C), u(0x30)
        disable = f(0x40)
        name = c[0x44:0x44 + 0x1C].split(b"\0", 1)[0].decode("latin1", "replace")
        n = (ve - vb) // 4 if vb and ve >= vb else 0
        checks = result["checks"] = {
            "placement_count": placement_count,
            "cap100": cap100, "cap50": cap50, "staging_count": staging,
            "vector_begin": hex(vb), "vector_end": hex(ve),
            "vector_count": n, "disable": disable, "path": name,
            "count_matches_vector": placement_count == n,
            "caps_ok": cap100 == 100 and cap50 == 50,
            "disable_ok": disable == 0,
        }
        print(f"  placement_count={placement_count} vector={n} "
              f"cap100={cap100} cap50={cap50} staging={staging} "
              f"disable={disable} path={name!r}")
        flags = 0
        groups = {}
        f6_values = set()
        for i in range(min(n, 5000)):
            ep = r.u32(vb + i * 4)
            e = r.bytes(ep, 0x24)
            try:
                x, y, w, h, sorty, f6, flag, grp, frm = struct.unpack_from(
                    "<9i", e, 0)
            except struct.error:
                break
            flags += 1 if flag else 0
            groups[grp] = groups.get(grp, 0) + 1
            f6_values.add(f6)
        result["elements"] = {
            "checked": min(n, 5000),
            "flag_1": flags,
            "groups": groups,
            "f6_sample": sorted(f6_values)[:20],
        }
        print(f"  elements checked={min(n, 5000)} flag=1:{flags} "
              f"groups={groups} f6_sample={sorted(f6_values)[:12]}")
        # key bytes in live module vs pinned snapshot
        same = diff = 0
        details = {}
        if snap_path and os.path.exists(snap_path):
            snap = open(snap_path, "rb").read()
            for key, va in KEY_VAS.items():
                off = va - 0x400000
                live = r.bytes(base + off, 16)
                pin = snap[off:off + 16]
                ok = live == pin
                details[key] = {"va": hex(va), "same": ok,
                                "live": live.hex(), "pinned": pin.hex()}
                same += ok
                diff += not ok
        result["key_bytes"] = details
        result["key_bytes_same"] = same
        result["key_bytes_diff"] = diff
        print(f"  key bytes: {same} same / {diff} differ")
        ok = (placement_count > 0 and n == placement_count and
              cap100 == 100 and cap50 == 50 and disable == 0 and
              diff == 0)
        result["pass"] = ok
        out = os.path.join(OUT_DIR, f"mask-peek-{sha[:16]}.json")
        os.makedirs(OUT_DIR, exist_ok=True)
        json.dump(result, open(out, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=1)
        print("saved:", out)
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    except Exception as exc:  # noqa: BLE001
        result["errors"].append(f"{type(exc).__name__}: {exc}")
        print("ERROR:", result["errors"][-1])
        return 1
    finally:
        r.close()


if __name__ == "__main__":
    sys.exit(main())
