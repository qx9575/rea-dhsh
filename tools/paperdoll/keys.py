"""APCI layer-key index（perinfo.pak 1B/12B blob）。只读。"""
from __future__ import annotations

import os
from collections import defaultdict
from pathlib import Path

from dhsh.formats import apci as dapci

from art_pipeline.common import PakReader


class LayerKeyIndex:
    def __init__(self, reader: PakReader):
        self._keys: dict[tuple[str, int, int], dict[int, list[int]]] = {}
        # perinfo.pak 是 APCI 容器，不是 PAKF；直接读原始文件。
        perinfo = Path(reader.shdata) / "perinfo.pak"
        if not perinfo.is_file():
            return
        data = perinfo.read_bytes()
        for (g, gid, r, rid, name, s, a, b, m, header, i,
             x, y, off, size) in dapci.iter_items(data):
            action = header & 0xFFFF
            bpp = (x >> 24) & 0xFF
            slot = self._keys.setdefault((name.lower(), model_key_gid(a), action), {})
            if bpp == 1:
                slot[i] = list(data[off:off + size])
            elif bpp == 12:
                slot[i] = [
                    int.from_bytes(data[off + 4 + 12 * k:off + 6 + 12 * k], "little")
                    for k in range(size // 12)
                ]
            else:
                slot[i] = []

    def keys_for(self, pak: str, part: str, model: int, action: int,
                 direction: int) -> list[int]:
        family = os.path.splitext(pak)[0]
        names = [f"{family}\\{part}", f"{family}\\{part.lower()}",
                 f"{family}\\{part.upper()}", family]
        for name in names:
            rec = self._keys.get((name.lower(), model, action))
            if rec:
                return rec.get(direction, [])
        return []


def model_key_gid(value: int) -> int:
    return value
