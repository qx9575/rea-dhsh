"""Read-only live verification of the Game.exe cover-mask container.

Attaches nothing; uses OpenProcess(PROCESS_VM_READ) + ReadProcessMemory only.
Dumps the mask container at *(DAT_00ab91b0 + 0x88) and its vector<Elem*>:
  container+0x08  staging-stack count
  container+0x10/+0x14  staging-record array begin/end
  container+0x28/+0x2c/+0x30  vector begin/end/cap
  container+0x40  disable flag (expect 0 = masks always on)
  each 44-byte Elem: +0x00 x, +0x04 y, +0x08 w, +0x0c h,
                     +0x10 sortY, +0x14 f6, +0x18 flag, +0x1c group, +0x20 frame

Usage: python tools/kimi_peek_masks.py [pid] [--watch N]
"""
import ctypes
import struct
import sys
import time

PROCESS_VM_READ = 0x10
PROCESS_QUERY_INFORMATION = 0x400

GLOBAL_PTR = 0x00AB91B0          # DAT_00ab91b0 = pointer to app struct
CONTAINER_OFF = 0x48 + 0x40      # app+0x48 = map; map+0x40 => app+0x88

k32 = ctypes.windll.kernel32


def find_pid(name="Game.exe"):
    k32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    if snap == ctypes.c_void_p(-1).value:
        return None
    class ENTRY(ctypes.Structure):
        _fields_ = [("dwSize", ctypes.c_ulong), ("cntUsage", ctypes.c_ulong),
                    ("th32ProcessID", ctypes.c_ulong), ("th32DefaultHeapID", ctypes.c_void_p),
                    ("th32ModuleID", ctypes.c_ulong), ("cntThreads", ctypes.c_ulong),
                    ("th32ParentProcessID", ctypes.c_ulong), ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", ctypes.c_ulong), ("szExeFile", ctypes.c_char * 260)]
    e = ENTRY()
    e.dwSize = ctypes.sizeof(ENTRY)
    pid = None
    ok = k32.Process32First(ctypes.c_void_p(snap), ctypes.byref(e))
    while ok:
        if e.szExeFile.decode(errors="ignore").lower() == name.lower():
            pid = e.th32ProcessID
            break
        ok = k32.Process32Next(ctypes.c_void_p(snap), ctypes.byref(e))
    k32.CloseHandle(ctypes.c_void_p(snap))
    return pid


class Reader:
    def __init__(self, pid):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.h:
            raise OSError("OpenProcess failed, err=%d" % ctypes.GetLastError())

    def u32(self, addr):
        buf = ctypes.c_ulong(0)
        n = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr),
                                     ctypes.byref(buf), 4, ctypes.byref(n)) or n.value != 4:
            raise OSError("RPM u32 @%08x failed, err=%d" % (addr, ctypes.GetLastError()))
        return buf.value

    def bytes(self, addr, size):
        buf = (ctypes.c_ubyte * size)()
        n = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr),
                                     buf, size, ctypes.byref(n)) or n.value != size:
            raise OSError("RPM @%08x+%x failed" % (addr, size))
        return bytes(buf)


def dump(r, verbose=True):
    base = GLOBAL_PTR
    app = r.u32(base)
    container = r.u32(app + CONTAINER_OFF)
    print("app *(0x%08x) = 0x%08x" % (base, app))
    print("container *(app+0x88) = 0x%08x" % container)
    if container == 0:
        print("container is NULL (not in a map scene?)")
        return
    c = r.bytes(container, 0x50)
    f = lambda o: struct.unpack_from("<i", c, o)[0]
    fu = lambda o: struct.unpack_from("<I", c, o)[0]
    print("  +0x00 init100=%d  +0x04 init50=%d  +0x08 staging_count=%d" % (f(0), f(4), f(8)))
    print("  +0x10 rec_begin=0x%08x  +0x14 rec_end=0x%08x  +0x18=%08x" % (fu(0x10), fu(0x14), fu(0x18)))
    vb, ve, vc = fu(0x28), fu(0x2c), fu(0x30)
    print("  +0x28 vec_begin=0x%08x  +0x2c vec_end=0x%08x  +0x30 vec_cap=0x%08x" % (vb, ve, vc))
    print("  +0x40 disable_flag=%d  +0x44 byte=%d" % (f(0x40), c[0x44]))
    n = (ve - vb) // 4 if vb and ve >= vb else 0
    print("  vector elems: %d" % n)
    if not verbose or n == 0 or n > 100000:
        return
    flags_on = 0
    groups = {}
    for i in range(n):
        ep = r.u32(vb + i * 4)
        e = r.bytes(ep, 0x24)
        x, y, w, h, sorty, f6, flag, grp, frm = struct.unpack_from("<9i", e, 0)
        flags_on += 1 if flag else 0
        groups.setdefault(grp, [0, 0])
        groups[grp][0] += 1
        groups[grp][1] += 1 if flag else 0
        if i < 12 or flag:
            print("  [%3d] @%08x x=%d y=%d w=%d h=%d sortY=%d f6=%d flag=%d group=%d frame=%d"
                  % (i, ep, x, y, w, h, sorty, f6, flag, grp, frm))
    print("  flag=1 total: %d / %d" % (flags_on, n))
    print("  groups (group: elems/flag_on):",
          {g: "%d/%d" % (v[0], v[1]) for g, v in sorted(groups.items())})


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else find_pid()
    if not pid:
        print("Game.exe not found")
        return 1
    print("pid=%d" % pid)
    r = Reader(pid)
    watch = None
    if "--watch" in sys.argv:
        watch = int(sys.argv[sys.argv.index("--watch") + 1])
    if watch:
        for _ in range(watch):
            dump(r, verbose=False)
            print("---")
            time.sleep(1)
    else:
        dump(r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
