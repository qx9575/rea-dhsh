"""带缓存索引的 PAK 读取器（paperdoll 专用，不修改 art_pipeline 现有实现）。

`art_pipeline.common.PakReader.find` 对每个 pak 线性扫描；批量校验 272 条预设时
会重复扫描数十万条索引。这里在首次访问某 pak 时建立 path/basename 字典，
保持与 `PakReader.find` 相同的“先精确路径、后 basename”语义。
"""
from __future__ import annotations

from dhsh import nz as dnz

from art_pipeline.common import PakReader


class CachedPakReader:
    def __init__(self, base: PakReader):
        self._base = base
        self._rows: dict[str, list[tuple[str, str, int, int]]] = {}
        self._exact: dict[str, dict[str, tuple[str, str, int, int]]] = {}
        self._basename: dict[str, dict[str, tuple[str, str, int, int]]] = {}

    @property
    def shdata(self):
        return self._base.shdata

    def _ensure(self, pak_name: str) -> None:
        if pak_name in self._rows:
            return
        rows = self._base._ensure(pak_name)  # noqa: SLF001 - 复用现有索引
        exact: dict[str, tuple[str, str, int, int]] = {}
        base: dict[str, tuple[str, str, int, int]] = {}
        for row in rows:
            exact.setdefault(row[0].replace("\\", "/").lower(), row)
            base.setdefault(row[1].lower(), row)
        self._rows[pak_name] = rows
        self._exact[pak_name] = exact
        self._basename[pak_name] = base

    def entries(self, pak_name: str) -> list[tuple[str, str, int, int]]:
        self._ensure(pak_name)
        return self._rows[pak_name]

    def find(self, pak_name: str, logical_path: str):
        target = logical_path.replace("\\", "/").lower()
        basename = target.rsplit("/", 1)[-1]
        self._ensure(pak_name)
        hit = self._exact[pak_name].get(target)
        if hit is None:
            hit = self._basename[pak_name].get(basename)
        return hit

    def read(self, pak_name: str, logical_path: str) -> tuple[str, bytes] | None:
        hit = self.find(pak_name, logical_path)
        if hit is None:
            return None
        path, _name, offset, size = hit
        with open(self._base.shdata / pak_name, "rb") as fh:
            fh.seek(offset)
            raw = fh.read(size)
        header = dnz.parse_header(raw)
        if header is None:
            payload = raw
        elif header.stored:
            payload = raw[dnz.HEADER_SIZE:]
        else:
            payload = dnz.decompress(header.kind, raw[dnz.HEADER_SIZE:], header.size1)
        return path, payload
