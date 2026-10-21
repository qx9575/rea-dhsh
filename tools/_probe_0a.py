#!/usr/bin/env python
"""Quick prototype for NZ type 0x0A (16-bit LZW variant) validation."""
import csv
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def decode_lzw10(payload):
    n = len(payload) // 2
    toks = struct.unpack('<%dH' % n, payload[:n * 2])
    if not toks:
        return b''
    if toks[0] == 0x100:
        return b''
    first = toks[0]
    if first > 0xFF:
        raise ValueError('first token %02x not literal' % first)

    prefix = [-1] * 65536
    suffix = [0] * 65536
    for c in range(256):
        suffix[c] = c

    out = bytearray([first])
    prev = first
    nxt = 0x101

    def expand(code):
        stack = []
        while code >= 0x100:
            stack.append(suffix[code])
            code = prefix[code]
            if code < 0:
                raise ValueError('bad prefix chain')
        stack.append(code)
        stack.reverse()
        return stack

    def first_char(code):
        guard = 0
        while code >= 0x100:
            code = prefix[code]
            guard += 1
            if code < 0 or guard > 0x10000:
                raise ValueError('bad prefix chain')
        return code

    for cur in toks[1:]:
        if cur == 0x100:
            break
        if cur < nxt:
            s = expand(cur)
        elif cur == nxt:
            fc = first_char(prev)
            s = expand(prev) + [fc]
        else:
            raise ValueError('code %d > next %d' % (cur, nxt))
        out.extend(s)
        if nxt <= 0xFFFF:
            prefix[nxt] = prev
            suffix[nxt] = s[0]
            nxt += 1
        prev = cur
    return bytes(out)


def main():
    manifest = os.path.join(ROOT, 'manifests', 'samples-compressed.csv')
    rows = [r for r in csv.DictReader(open(manifest, encoding='utf-8-sig'))
            if r['nz_type'] == '0x000a']
    print('0x0A sample rows:', len(rows))
    ok = fail = 0
    for r in rows:
        rel = r['out_path'].replace('..' + chr(92), '').replace(chr(92), '/')
        path = os.path.join(ROOT, rel)
        raw = open(path, 'rb').read()
        assert raw[:2] == b'NZ'
        typ, s1, s2 = struct.unpack('<HII', raw[2:12])
        out = decode_lzw10(raw[12:12 + s2])
        good = len(out) == s1
        if good:
            ok += 1
        else:
            fail += 1
        if good and ok > 30:
            continue
        print('%-16s %-42s type=%04x s1=%-8d s2=%-8d out=%-8d %s head=%s'
              % (r['pak'], r['path'], typ, s1, s2, len(out),
                 'OK' if good else 'LEN_MISMATCH', out[:12].hex(' ')))
    print('ok', ok, 'fail', fail)


if __name__ == '__main__':
    main()
