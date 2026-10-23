"""Verify CTI table2 semantics: hue_rotate(table2, 0) == table1 ?

Implements the exact algorithm of Game.exe FUN_00824cc0 (hue-only HSV
rotation, wheel=252, sector=42) and compares its RGB565 output against
table1 of real clrtb.cti files.

By default only gray CTIs (all table2 S==0) are checked, because that is
the claim being verified: dyeable equipment bases are gray and their
table1 equals hue0(table2) exactly.  Use --all to sample colored CTIs as
well (their table1 is art-baked and is not expected to match).

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
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--all", action="store_true",
                    help="sample all CTIs (default: only gray S==0 CTIs)")
    ap.add_argument("--sample", type=int, default=30)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    idx = json.load(open("manifests/batches/cti-index.json"))["items"]
    paths = sorted({row[1] for row in idx})
    random.seed(args.seed)
    random.shuffle(paths)
    checked = 0
    total_bad = 0
    gray_files = 0
    for p in paths:
        if checked >= args.sample:
            break
        try:
            t1, t2 = load_cti(p)
        except Exception as e:
            print("SKIP", p, e)
            continue
        gray = all(e[1] == 0 for e in t2[1:])
        if not args.all and not gray:
            continue
        gray_files += 1
        bad = []
        for i in range(1, 256):
            h, s, v, _ = t2[i]
            want = hsv565(h, s, v, 0)
            if want != t1[i]:
                bad.append((i, t1[i], want, t2[i]))
        status = "OK " if not bad else "DIFF(%d)" % len(bad)
        total_bad += len(bad)
        checked += 1
        print("%s %s%s" % (status, p.split("extracted")[-1],
                           " [gray]" if gray else ""))
        for b in bad[:3]:
            print("    idx=%d table1=%04x gen=%04x hsv=%s" % b)
    print("checked %d files (%d gray), mismatched entries: %d"
          % (checked, gray_files, total_bad))
    return 0


if __name__ == "__main__":
    sys.exit(main())
