"""Read runtime decoder state from a running Game.exe process.

Read-only: OpenProcess(PROCESS_VM_READ|QUERY_INFORMATION) + ReadProcessMemory.
No debugger, no injection.  Run while the game client is running.

Dumps and checks:
  [0xb0cfd8]  active v2 converter fn ptr   (0x8a3590=MMX/555, 0x8a32d0=MMX/565,
                                             0x8a3960=scalar)
  [0xb0cbc0]  active pixel helper fn ptr   (0x8a3840=555, 0x8a38d0=565)
  [0xb0c9b8]  active IDCT fn ptr           (0x8a8780=MMX, 0x8a8630=scalar)
  [0xb0cec8]  active R mask pair           (0xfc00fc00=555, 0xf800f800=565)
  tables      0xb0ced0/0xb0cdc8/0xb0cbc8/0xb0ccc8, 256 bytes each,
              compared against predicted builds (rne vs trunc, bias 1.0 vs 0.0)
  0xab6350    IDCT prescale table sanity check vs file bytes
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys

import numpy as np

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

_k32 = ctypes.windll.kernel32
_psapi = ctypes.windll.psapi
# explicit 64-bit-clean prototypes (wintypes.HMODULE is 32-bit on py<=3.12)
_k32.OpenProcess.restype = wt.HANDLE
_k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
_k32.ReadProcessMemory.restype = wt.BOOL
_k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p,
                                   ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
_k32.CloseHandle.argtypes = [wt.HANDLE]
_psapi.EnumProcesses.restype = wt.BOOL
_psapi.EnumProcesses.argtypes = [ctypes.POINTER(wt.DWORD), wt.DWORD,
                                 ctypes.POINTER(wt.DWORD)]
_psapi.EnumProcessModules.restype = wt.BOOL
_psapi.EnumProcessModules.argtypes = [wt.HANDLE,
                                      ctypes.POINTER(ctypes.c_void_p),
                                      wt.DWORD, ctypes.POINTER(wt.DWORD)]
_psapi.GetModuleFileNameExW.restype = wt.DWORD
_psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p,
                                        wt.LPWSTR, wt.DWORD]

# VAs in the original (pre-VMProtect) address space; runtime addresses are
# module_base + (va - 0x400000)
VA_BASE = 0x400000
PTRS = {"converter[0xb0cfd8]": 0xB0CFD8, "pixel_helper[0xb0cbc0]": 0xB0CBC0,
        "idct[0xb0c9b8]": 0xB0C9B8, "Rmask[0xb0cec8]": 0xB0CEC8,
        "Gmask[0xb0cfd0]": 0xB0CFD0}
TABLES = {"T_R[0xb0ced0]": 0xB0CED0, "T_Gb[0xb0cdc8]": 0xB0CDC8,
          "T_Gr[0xb0cbc8]": 0xB0CBC8, "T_B[0xb0ccc8]": 0xB0CCC8}
FN_NAMES = {0x8A3590: "0x8a3590 MMX RGB555 v2", 0x8A32D0: "0x8a32d0 MMX RGB565 v2",
            0x8A3960: "0x8a3960 scalar v2", 0x8A3840: "0x8a3840 RGB555 helper",
            0x8A38D0: "0x8a38d0 RGB565 helper", 0x8A8780: "0x8a8780 MMX IDCT",
            0x8A8630: "0x8a8630 scalar IDCT"}


def find_game():
    pids = (wt.DWORD * 4096)()
    needed = wt.DWORD()
    if not _psapi.EnumProcesses(pids, ctypes.sizeof(pids),
                                ctypes.byref(needed)):
        raise RuntimeError("EnumProcesses failed")
    for pid in pids[: needed.value // 4]:
        if pid == 0:
            continue
        h = _k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                             False, pid)
        if not h:
            continue
        mods = (ctypes.c_void_p * 1024)()
        mneed = wt.DWORD()
        ok = _psapi.EnumProcessModules(h, mods, ctypes.sizeof(mods),
                                       ctypes.byref(mneed))
        if ok:
            for mod in mods[: mneed.value // ctypes.sizeof(ctypes.c_void_p)]:
                if not mod:
                    continue
                name = ctypes.create_unicode_buffer(260)
                _psapi.GetModuleFileNameExW(h, mod, name, 260)
                if name.value.lower().endswith("game.exe"):
                    return pid, h, mod
        _k32.CloseHandle(h)
    return None, None, None


def read(h, addr, size):
    buf = (ctypes.c_char * size)()
    got = ctypes.c_size_t()
    if not _k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size,
                                  ctypes.byref(got)):
        raise RuntimeError(f"ReadProcessMemory failed at {addr:#x}")
    return bytes(buf[: got.value])


def rne(x):
    return int(np.rint(x))


def trunc_(x):
    return int(x) if x >= 0 else -int(-x)  # toward zero


def pred_table(kind, rounding, bias):
    out = np.empty(256, dtype=np.uint8)
    for i in range(256):
        v = i - 256 if i >= 128 else i
        f = rne if rounding == "rne" else trunc_
        if kind == "R":
            val = f(1.402 * v + bias)
        elif kind == "Gb":
            val = f(bias - 0.34414 * v)
        elif kind == "Gr":
            val = f(bias - 0.71414 * v)
        else:
            val = f(1.772 * v + bias)
        out[i] = (val >> 1) & 0xFF
    return out


def main() -> int:
    pid, h, base = find_game()
    if not h:
        print("Game.exe not found (or access denied - anti-cheat?).")
        return 1
    print(f"Game.exe pid={pid}, module base={base:#x}")

    def addr(va):
        return base + (va - VA_BASE)

    for name, va in PTRS.items():
        (val,) = np.frombuffer(read(h, addr(va), 4), dtype="<u4")
        # function pointers are stored as original VAs already
        fn = FN_NAMES.get(int(val), "?")
        extra = f" -> {fn}" if val >= VA_BASE else ""
        print(f"{name}: {val:#010x}{extra}")

    kinds = {"T_R[0xb0ced0]": "R", "T_Gb[0xb0cdc8]": "Gb",
             "T_Gr[0xb0cbc8]": "Gr", "T_B[0xb0ccc8]": "B"}
    for name, va in TABLES.items():
        mem = np.frombuffer(read(h, addr(va), 256), dtype=np.uint8)
        best = []
        for rounding in ("rne", "trunc"):
            for bias in (1.0, 0.0, 0.5):
                p = pred_table(kinds[name], rounding, bias)
                n = int((mem == p).sum())
                best.append((n, rounding, bias))
        best.sort(reverse=True)
        n, rounding, bias = best[0]
        print(f"{name}: best match rne-mode={rounding} bias={bias} "
              f"({n}/256 bytes identical)")
        if n < 256:
            p = pred_table(kinds[name], rounding, bias)
            bad = np.nonzero(mem != p)[0][:8]
            print(f"   first diffs at idx {bad.tolist()}: "
                  f"mem={mem[bad].tolist()} pred={p[bad].tolist()}")

    mem = read(h, addr(0xAB6350), 128)
    ref = open(r"D:\software\dhsh\shdata\Game.exe", "rb").read()[0x6B6350:0x6B6350 + 128]
    print(f"IDCT prescale table in memory == file bytes: {mem == ref}")
    ctypes.windll.kernel32.CloseHandle(h)
    return 0


if __name__ == "__main__":
    sys.exit(main())
