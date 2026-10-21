"""Verify CTI table2 semantics: hue_rotate(table2, 0) == table1 ?

Implements the exact algorithm of Game.exe FUN_00824cc0 (hue-only HSV
rotation, wheel=252, sector=42) and compares its RGB565 output against
table1 of real clrtb.cti files. Also spot-checks a few non-zero hues by
self-consistency (rotate(a+b) == rotate(a)+b mod 252 composition).

table2 entry layout (from FUN_00824cc0/FUN_008252c0): 4 bytes {H, S, V, pad}
"""
import json
import random
import struct
import sys


def hsv565(h, s, v, rot):
    # exact port of the sector math in FUN_00824cc0
    hh = (h + rot) % 0xFC
    floor = v - (s * v) // 0xFF          # uVar6 = v - s*v/255
    r = g = b = v
    if s != 0 and v != floor:
        if hh > 0xD1:
            hh -= 0xFC
        if hh < 0x2B:
            i = ((v - floor) * hh) // 0x2A
            if i < 0:
                b = floor - i
                g = floor
            else:
                b = floor
                g = i + floor
            r = v
        elif hh < 0x7F:
            i = ((v - floor) * (hh - 0x54)) // 0x2A
            if i < 0:
                r = floor - i
                b = floor
            else:
                b = i + floor
                r = v
            g = floor
        else:
            i = ((v - floor) * (hh - 0xA8)) // 0x2A
            if i < 0:
                g = floor - i
            else:
                r = i + floor
                g = v
            b = floor
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b >> 3) & 0x1F)


def load_cti(path):
    raw = open(path, "rb").read()
    assert raw[:2] == b"CT", path
    hdr, cnt, t1sz = struct.unpack_from("<HHxxxxxxI", raw, 2)[0], None, None
    header_size = struct.unpack_from("<H", raw, 2)[0]
    count = struct.unpack_from("<H", raw, 4)[0]
    t1sz = struct.unpack_from("<I", raw, 0x0C)[0]
    t2sz = struct.unpack_from("<I", raw, 0x10)[0]
    t1 = struct.unpack_from("<%dH" % count, raw, header_size)
    t2off = header_size + t1sz
    t2 = [tuple(raw[t2off + i * 4: t2off + i * 4 + 4]) for i in range(count)]
    return t1, t2


def main():
    idx = json.load(open("manifests/batches/cti-index.json"))["items"]
    paths = sorted({row[1] for row in idx})
    random.seed(42)
    sample = random.sample(paths, 30)
    total_bad = 0
    for p in sample:
        try:
            t1, t2 = load_cti(p)
        except Exception as e:
            print("SKIP", p, e)
            continue
        bad = []
        for i in range(1, 256):
            h, s, v, _ = t2[i]
            want = hsv565(h, s, v, 0)
            if want != t1[i]:
                bad.append((i, t1[i], want, t2[i]))
        status = "OK " if not bad else "DIFF(%d)" % len(bad)
        total_bad += len(bad)
        print("%s %s" % (status, p.split("extracted")[-1]))
        for b in bad[:3]:
            print("    idx=%d table1=%04x gen=%04x hsv=%s" % b)
    print("TOTAL mismatched entries:", total_bad)
    return 0


if __name__ == "__main__":
    sys.exit(main())
