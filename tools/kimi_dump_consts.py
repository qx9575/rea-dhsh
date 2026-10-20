"""Dump constants used by the private-JPEG decoder from Game.exe.

Reads (VA -> file offset via PE section headers):
    0xab6350  64 x u16   IDCT prescale table (paired 1:1 with coeff buffer)
    0xab6330  4 x u16    AAN rotation constant K0
    0xab6338  4 x u16    AAN rotation constant K1
    0xab6340  4 x u16    AAN rotation constant K2
    0xab6348  4 x u16    AAN rotation constant K3
    0xab6320  4 x u16    rounding bias (paddw before psraw 2)
    0xab6308  qword      level-shift addend for Y (paddb)
    0xab6310  qword      mask after psrlq 1
    0xa46498  double     color K1 (R term, x Cr)
    0xa46490  double     color K2 (G term, x Cb)
    0xa46488  double     color K3 (G term, x Cr)
    0xa46480  double     color K4 (B term, x Cb)
    0x984180  double     color bias B
    0x939730  code bytes float->int helper (to confirm __ftol semantics)
"""
import struct
import sys

PATH = r"D:\software\dhsh\shdata\Game.exe"


def va_to_offset(pe: bytes, va: int) -> int:
    # Dump VAs use the pre-VMProtect image base 0x400000; in this PE
    # vaddr == raw offset for both .text and .rdata, so off = va - 0x400000.
    return va - 0x400000


def main() -> None:
    pe = open(PATH, "rb").read()

    def rd(va: int, n: int) -> bytes:
        return pe[va_to_offset(pe, va):va_to_offset(pe, va) + n]

    def words(va: int, n: int):
        b = rd(va, n * 2)
        return list(struct.unpack("<%dH" % n, b))

    print("== IDCT prescale table 0xab6350 (64 x u16, 8x8 natural order) ==")
    t = words(0xAB6350, 64)
    for r in range(8):
        print("  " + " ".join(f"{t[r * 8 + c]:5d}" for c in range(8)))
    as_f = [[t[r * 8 + c] / 32768.0 for c in range(8)] for r in range(8)]
    print("  as Q15 float:")
    for r in range(8):
        print("  " + " ".join(f"{as_f[r][c]:9.5f}" for c in range(8)))

    for name, va in (("K0 0xab6330", 0xAB6330), ("K1 0xab6338", 0xAB6338),
                     ("K2 0xab6340", 0xAB6340), ("K3 0xab6348", 0xAB6348),
                     ("bias 0xab6320", 0xAB6320)):
        w = words(va, 4)
        print(f"{name}: {w}  as Q15: {[x / 32768.0 for x in w]}"
              f"  as Q16: {[x / 65536.0 for x in w]}")

    print("0xab6308 qword:", rd(0xAB6308, 8).hex())
    print("0xab6310 qword:", rd(0xAB6310, 8).hex())

    print("== color constants ==")
    for name, va in (("K1 0xa46498", 0xA46498), ("K2 0xa46490", 0xA46490),
                     ("K3 0xa46488", 0xA46488), ("K4 0xa46480", 0xA46480),
                     ("B  0x984180", 0x984180)):
        (d,) = struct.unpack("<d", rd(va, 8))
        print(f"{name}: {d!r}")

    print("== 0x939730 first bytes (float->int helper) ==")
    print(rd(0x939730, 32).hex())


if __name__ == "__main__":
    sys.exit(main())
