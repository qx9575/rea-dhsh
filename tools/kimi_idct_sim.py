"""Bit-exact simulation of the MMX IDCT at VA 0x8a8780 (Game.exe).

The function body is straight-line MMX, so this script parses the disassembly
listing (manifests/batches/special-jpg-code-dump.txt) and interprets every
instruction with exact x86 semantics:

    pmulhw   lane := (s16(a) * s16(b)) >> 16        (arithmetic, floors)
    paddsw/psubsw  saturating to [-32768, 32767]
    paddw    wrapping 16-bit add
    psraw    arithmetic shift (floors for negatives)
    psllw    logical shift left, result truncated to 16 bits
    packsswb saturate to [-128, 127], dst lanes then src lanes
    punpck*  standard interleave
    movd/movq to/from the 64x s16 coefficient buffer (byte offsets)

Then it validates the effective numeric behaviour against the textbook
float IDCT  f = (1/4) sum C(u)C(v) F cos cos  and reports the exact scale.
"""
from __future__ import annotations

import math
import re
import sys

import numpy as np

DUMP = "manifests/batches/special-jpg-code-dump.txt"

# ---------------------------------------------------------------- constants
TBL_ADDR = 0xAB6350
TABLE_WORDS = [  # 64 x u16, dumped from Game.exe (file offset = va - 0x400000)
    16384, 22725, 21407, 19266, 16384, 12873, 8867, 4520,
    22725, 31521, 29692, 26722, 22725, 17855, 12299, 6270,
    21407, 29692, 27969, 25172, 21407, 16819, 11585, 5906,
    19266, 26722, 25172, 22654, 19266, 15137, 10426, 5315,
    16384, 22725, 21407, 19266, 16384, 12873, 8867, 4520,
    12873, 17855, 16819, 15137, 25746, 20228, 13933, 7103,
    17734, 24598, 23170, 20853, 17734, 13933, 9597, 4892,
    18081, 25080, 23624, 21261, 18081, 14206, 9785, 4988,
]
K0330 = np.array([23170] * 4, dtype=np.int16)   # 0xab6330
K0338 = np.array([21407] * 4, dtype=np.int16)   # 0xab6338
K0340 = np.array([17734] * 4, dtype=np.int16)   # 0xab6340
K0348 = np.array([25080] * 4, dtype=np.int16)   # 0xab6348
BIAS0320 = np.array([1, 1, 2, 5], dtype=np.int16)  # 0xab6320
TABLE = np.array(TABLE_WORDS, dtype=np.int16)
CONSTS = {0xAB6330: K0330, 0xAB6338: K0338, 0xAB6340: K0340,
          0xAB6348: K0348, 0xAB6320: BIAS0320}


# ------------------------------------------------------------ mmx primitives
def s16(v):
    return np.asarray(v, dtype=np.int16)


def pmulhw(a, b):
    return ((a.astype(np.int32) * b.astype(np.int32)) >> 16).astype(np.int16)


def paddsw(a, b):
    return np.clip(a.astype(np.int32) + b.astype(np.int32),
                   -32768, 32767).astype(np.int16)


def psubsw(a, b):
    return np.clip(a.astype(np.int32) - b.astype(np.int32),
                   -32768, 32767).astype(np.int16)


def paddw(a, b):
    return ((a.astype(np.int32) + b.astype(np.int32)) & 0xFFFF).astype(np.int16)


def psllw(a, n):
    return ((a.astype(np.int32) << n) & 0xFFFF).astype(np.int16)


def psraw(a, n):
    return (a >> n).astype(np.int16)          # numpy int16 >> is arithmetic


def packsswb(a, b):
    # operands are always 4 words each; a may be a previously packed
    # register held as 8 int8 lanes -- reinterpret those bytes as words
    if getattr(a, "dtype", None) == np.int8:
        a = a.view(np.int16)
    if getattr(b, "dtype", None) == np.int8:
        b = b.view(np.int16)
    lo = np.clip(a.astype(np.int16), -128, 127).astype(np.int8)
    hi = np.clip(b.astype(np.int16), -128, 127).astype(np.int8)
    return np.concatenate([lo, hi])


# ------------------------------------------------------------ interpreter
class Machine:
    def __init__(self):
        self.mm = [np.zeros(4, dtype=np.int16) for _ in range(8)]
        self.buf = np.zeros(64, dtype=np.int16)       # esi: 128 bytes
        self.scratch = {}                              # 0xcb03xx qwords

    def load(self, op: str):
        m = re.fullmatch(r"qword ptr \[([^\]]+)\]", op)
        assert m, op
        expr = m.group(1)
        if "+" in expr:
            base, off = expr.split("+")
            base, off = base.strip(), int(off.strip(), 16)
        else:
            base, off = expr.strip(), 0
            if base.startswith("0x"):
                base, off = "", int(base, 16)
        if base == "ecx":
            addr = TBL_ADDR + off
            i = (addr - TBL_ADDR) // 2
            return TABLE[i:i + 4].copy()
        if base == "esi":
            return self.buf[off // 2:off // 2 + 4].copy()
        if base == "":
            if off in CONSTS:
                return CONSTS[off].copy()
            return self.scratch.setdefault(off, np.zeros(4, dtype=np.int16))
        raise ValueError(op)

    def store(self, op: str, val, words: int):
        m = re.fullmatch(r"(?:qword|dword) ptr \[([^\]]+)\]", op)
        assert m, op
        expr = m.group(1)
        if "+" in expr:
            base, off = expr.split("+")
            base, off = base.strip(), int(off.strip(), 16)
        else:
            base, off = expr.strip(), 0
            if base.startswith("0x"):
                base, off = "", int(base, 16)
        assert base in ("esi", ""), op
        if getattr(val, "dtype", None) == np.int8:
            nbytes = 4 if words == 2 else 8
            assert base == "esi", op
            self.buf.view(np.int8)[off:off + nbytes] = val[:nbytes]
            return
        if base == "esi":
            self.buf[off // 2:off // 2 + words] = val[:words]
        else:
            q = self.scratch.setdefault(off, np.zeros(4, dtype=np.int16))
            q[:words] = val[:words]

    def run(self, insns):
        for addr, text in insns:
            parts = text.split(None, 1)
            op = parts[0]
            args = [a.strip() for a in parts[1].split(",")] if len(parts) > 1 else []
            if op in ("push", "pop", "ret", "emms", "nop", "int3", "cld"):
                continue
            if op == "lea":            # lea ecx, [0xab6350]
                continue
            if op == "mov":            # mov esi, dword ptr [esp + 8]
                continue
            dst = int(args[0][2]) if args and args[0].startswith("mm") else None
            if op == "movq" or op == "movd":
                if dst is not None:
                    if args[1].startswith("mm"):
                        self.mm[dst] = self.mm[int(args[1][2])].copy()
                    else:
                        self.mm[dst] = self.load(args[1])[:4]
                else:
                    self.store(args[0], self.mm[int(args[1][2])],
                               4 if op == "movq" else 2)
                continue
            src = self.mm[int(args[1][2])] if args[1].startswith("mm") \
                else (int(args[1], 16) if re.fullmatch(r"0x[0-9a-f]+|\d+", args[1])
                      else self.load(args[1]))
            if op == "pmulhw":
                self.mm[dst] = pmulhw(self.mm[dst], src)
            elif op == "paddsw":
                self.mm[dst] = paddsw(self.mm[dst], src)
            elif op == "psubsw":
                self.mm[dst] = psubsw(self.mm[dst], src)
            elif op == "paddw":
                self.mm[dst] = paddw(self.mm[dst], src)
            elif op == "psraw":
                self.mm[dst] = psraw(self.mm[dst], src)
            elif op == "psllw":
                self.mm[dst] = psllw(self.mm[dst], src)
            elif op == "packsswb":
                self.mm[dst] = packsswb(self.mm[dst], src)   # 8 int8 lanes
            elif op == "punpcklwd":
                a, b = self.mm[dst], src
                self.mm[dst] = np.array([a[0], b[0], a[1], b[1]], dtype=np.int16)
            elif op == "punpckhwd":
                a, b = self.mm[dst], src
                self.mm[dst] = np.array([a[2], b[2], a[3], b[3]], dtype=np.int16)
            elif op == "punpckldq":
                a, b = self.mm[dst], src
                self.mm[dst] = np.array([a[0], a[1], b[0], b[1]], dtype=np.int16)
            elif op == "punpckhdq":
                a, b = self.mm[dst], src
                if a.dtype == np.int8:      # post-packsswb: interleave high dwords
                    self.mm[dst] = np.concatenate([a[4:8], b[4:8]])
                else:
                    self.mm[dst] = np.array([a[2], a[3], b[2], b[3]],
                                            dtype=np.int16)
            else:
                raise ValueError(f"{addr}: unhandled {text}")
            if op != "packsswb" and self.mm[dst].dtype != np.int8:
                self.mm[dst] = s16(self.mm[dst])


def load_listing() -> list[tuple[str, str]]:
    lines = open(DUMP, encoding="utf-8").read().splitlines()
    out, on = [], False
    for ln in lines:
        if ln.startswith("====="):
            on = "0x8a8780_idct_mmx" in ln
            continue
        m = re.match(r"^([0-9a-f]{8}): (.+)$", ln)
        if on and m:
            out.append((m.group(1), m.group(2)))
    return out


INSNS = load_listing()


def idct_mmx(coeff64) -> np.ndarray:
    """coeff64: 64 ints (natural order, row-major) -> 64 int8 samples."""
    mac = Machine()
    mac.buf[:] = np.asarray(coeff64, dtype=np.int16)
    mac.run(INSNS)
    # output lands in the first 64 bytes of the buffer as int8
    return mac.buf.view(np.int8)[:64].copy()


# ------------------------------------------------------------ float reference
_COS = np.array([[math.cos((2 * x + 1) * u * math.pi / 16)
                  for u in range(8)] for x in range(8)])
_C = np.array([1 / math.sqrt(2)] + [1.0] * 7)
_T = _COS * _C[None, :]


def idct_ref(coeff64) -> np.ndarray:
    """Textbook: f = (1/4) sum C(u)C(v) F cos cos  (float64)."""
    f = np.asarray(coeff64, dtype=np.float64).reshape(8, 8)
    return (_T @ f @ _T.T) / 4.0


def main() -> int:
    print(f"parsed {len(INSNS)} instructions from listing")

    print("\n== DC gain (all outputs equal) ==")
    for v in (8, 64, 512, 2048, 8192, -2048):
        c = np.zeros(64, dtype=np.int64)
        c[0] = v
        out = idct_mmx(c).astype(np.int32)
        ref = v / 8.0
        print(f"  F0={v:6d}  sim={out[0]:6d} (min={out.min()}, max={out.max()})"
              f"  ref={ref:9.3f}  sim/ref={out[0] / ref:.6f}")

    print("\n== per-position impulse fit (least squares, big amplitude) ==")
    scales = []
    for p in range(64):
        c = np.zeros(64, dtype=np.int64)
        c[p] = 4000 if p else 24000
        out = idct_mmx(c).astype(np.float64).reshape(8, 8)
        ref = idct_ref(c).T
        mask = np.abs(ref) > 8.0
        s = float((out[mask] * ref[mask]).sum() / (ref[mask] ** 2).sum())
        scales.append(s)
    med = float(np.median(scales))
    print(f"  median S = {med:.8f}  (1/16 = {1 / 16:.8f})")
    print(f"  min = {min(scales):.8f}  max = {max(scales):.8f}")
    worst = sorted(range(64), key=lambda i: -abs(scales[i] - med))[:6]
    for i in worst:
        print(f"    pos {i:2d} (u={i % 8},v={i // 8}): S={scales[i]:.8f}")

    print("\n== global scale via LSQ on dense blocks ==")
    rng = np.random.default_rng(42)
    num = den = 0.0
    for _ in range(300):
        c = rng.integers(-300, 301, 64)
        out = idct_mmx(c).astype(np.float64)
        ref = idct_ref(c).T.reshape(-1)
        num += float((out * ref).sum())
        den += float((ref ** 2).sum())
    S = num / den
    print(f"  S = {S:.8f}   S*16 = {S * 16:.6f}")

    print("\n== elementwise: sim vs trunc/rint(S*ref), clipped ==")
    for label, gen in (("dense +-300", lambda: rng.integers(-300, 301, 64)),
                       ("sparse JPEG-like", None)):
        hist = {}
        total = 0
        mism_r = 0
        for _ in range(3000):
            if gen is not None:
                c = gen()
            else:
                c = np.zeros(64, dtype=np.int64)
                n = rng.integers(1, 10)
                idx = rng.choice(64, n, replace=False)
                c[idx] = rng.integers(-1500, 1501, n)
            out = idct_mmx(c).astype(np.int64)
            ref = (idct_ref(c).T * S).reshape(-1)
            tr = np.clip(np.floor(ref), -128, 127).astype(np.int64)
            rd = np.clip(np.rint(ref), -128, 127).astype(np.int64)
            d = out - tr
            for v in np.unique(d):
                hist[int(v)] = hist.get(int(v), 0) + int((d == v).sum())
            total += 64
            mism_r += int((out != rd).sum())
        exact = hist.get(0, 0)
        print(f"  {label}: vs trunc: exact {exact}/{total} "
              f"({100.0 * exact / total:.2f}%), err histogram {dict(sorted(hist.items()))}")
        print(f"  {label}: vs rint: mismatch {mism_r}/{total}")

    print("\n== saturation behaviour (large DC) ==")
    for v in (8192, 16384, 32767, -32768):
        c = np.zeros(64, dtype=np.int64)
        c[0] = v
        out = idct_mmx(c).astype(np.int32)
        print(f"  F0={v:6d} -> min {out.min()}, max {out.max()}"
              f"  (ref/16 = {v / 128:.1f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
