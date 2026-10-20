"""Empirically verify the v2 DC-slot scheme against real image data.

The disassembly of the MCU driver (0x823240, v2 branch 0x823488-0x82361c)
shows the six per-MCU block decodes receive these DC predictor pointers
into the 12-byte stack record (+6=f2, +8=f3, +0xa=f4):

    call 1-4 (Y0..Y3, comp0):  seed = &f2      (slot 0)
    call 5   (Cb, comp1):      seed = &f3      (slot 1)
    call 6   (Cr, comp2):      seed = &f4      (slot 2)

i.e. ALL four luma blocks share slot 0 -- NOT the 0/1/2/0/1/2 cycle used
by tools/dhsh/formats/special_jpg.py.  Both schemes agree at MCU boundaries
(which is why the descriptor chain check passed), but inside one MCU the
cycle scheme seeds Y1 from the *chroma* chain.

This script decodes a v2 file's luma DCs under both schemes and compares
within-image smoothness of the resulting DC planes.
"""
from __future__ import annotations

import sys

import numpy as np

sys.path.insert(0, "tools")
from dhsh.formats.special_jpg import parse, decode_block  # noqa: E402

PATH = (sys.argv[1] if len(sys.argv) > 1 else
        r"extracted\easy\YTFDL_2_q.jpg")


def luma_dc_plane(jpg, order):
    cols, rows = jpg.mcu_cols, jpg.mcu_rows
    # Y block grid: 2x2 blocks per MCU
    plane = np.zeros((rows * 2, cols * 2), dtype=np.int64)
    for i in range(rows * cols):
        my, mx = divmod(i, cols)
        ptr, bitoff, f2, f3, f4 = jpg.records[i]
        seeds = [int(f2), int(f3), int(f4)]
        state = (int(ptr), int(bitoff))
        for bi, (comp_i, slot) in enumerate(order):
            _coeff, dc, state = decode_block(jpg, state, comp_i, seeds[slot])
            seeds[slot] = dc
            if bi < 4:
                plane[my * 2 + bi // 2, mx * 2 + bi % 2] = dc
    return plane


def roughness(p):
    return (np.abs(np.diff(p, axis=0)).mean()
            + np.abs(np.diff(p, axis=1)).mean()) / 2.0


def main() -> int:
    jpg = parse(open(PATH, "rb").read())
    assert jpg.version == 2, "need a v2 file"
    wrong = [(0, 0), (0, 1), (0, 2), (0, 0), (1, 1), (2, 2)]   # current cycle
    right = [(0, 0), (0, 0), (0, 0), (0, 0), (1, 1), (2, 2)]   # per disassembly
    pw = luma_dc_plane(jpg, wrong)
    pr = luma_dc_plane(jpg, right)
    print(f"file: {PATH}  {jpg.width}x{jpg.height}")
    print(f"wrong scheme (cycle 0/1/2/0):  DC roughness = {roughness(pw):.1f}")
    print(f"right scheme (all slot 0):     DC roughness = {roughness(pr):.1f}")
    nd = int((pw != pr).sum())
    print(f"DC values differing: {nd}/{pw.size}")
    # within-MCU spread of the 4 luma DCs
    for name, p in (("wrong", pw), ("right", pr)):
        q = p.reshape(-1, 2, p.shape[1] // 2, 2)
        q = q.transpose(0, 2, 1, 3).reshape(-1, 4)
        print(f"{name}: median within-MCU Y-DC spread "
              f"{np.median(q.max(axis=1) - q.min(axis=1)):.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
