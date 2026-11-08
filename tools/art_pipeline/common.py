"""PAK 表读取、文本解码、容错 CSV 解析。只读。"""
from __future__ import annotations

import csv
import io
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Sequence

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from dhsh import pak as dpak  # noqa: E402
from dhsh import nz as dnz  # noqa: E402
from dhsh.formats.scp import decode_scp  # noqa: E402

DEFAULT_SHDATA = Path(os.environ.get("DHSH_SHDATA", r"D:\software\dhsh\shdata"))
DEFAULT_INVENTORY = REPO_ROOT / "kimi_viewer_data" / "pak_inventory.json"
DEFAULT_OUT = REPO_ROOT / "extracted" / "art_pipeline"
DEFAULT_DOCS = REPO_ROOT / "docs" / "art-pipeline"


@dataclass(frozen=True)
class TableText:
    key: str
    pak: str
    path: str
    text: str
    comments: tuple[str, ...]
    rows: tuple[tuple[str, ...], ...]


class PakReader:
    """按需扫描指定 PAK，缓存索引。只读取，不写原目录。"""

    def __init__(self, shdata: Path | str = DEFAULT_SHDATA):
        self.shdata = Path(shdata)
        self._index: dict[str, list[tuple[str, str, int, int]]] = {}

    def _ensure(self, pak_name: str) -> list[tuple[str, str, int, int]]:
        if pak_name in self._index:
            return self._index[pak_name]
        fp = self.shdata / pak_name
        rows: list[tuple[str, str, int, int]] = []
        if fp.is_file():
            for pe in dpak.iter_pak(str(fp)):
                rows.append((pe.path, pe.name, pe.offset, pe.size))
        self._index[pak_name] = rows
        return rows

    def find(self, pak_name: str, logical_path: str) -> tuple[str, str, int, int] | None:
        target = logical_path.replace("\\", "/").lower()
        base = target.rsplit("/", 1)[-1]
        rows = self._ensure(pak_name)
        for row in rows:
            if row[0].replace("\\", "/").lower() == target:
                return row
        for row in rows:
            if row[1].lower() == base:
                return row
        return None

    def read(self, pak_name: str, logical_path: str) -> tuple[str, bytes] | None:
        hit = self.find(pak_name, logical_path)
        if hit is None:
            return None
        path, _name, offset, size = hit
        with open(self.shdata / pak_name, "rb") as fh:
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


def decode_text(data: bytes) -> str:
    """优先按游戏文本规则解码，失败时回退常见编码。"""
    try:
        text = decode_scp(data)[0]
        if text:
            return text.replace("﻿", "")
    except Exception:  # noqa: BLE001
        pass
    for encoding in ("utf-8-sig", "gb18030", "utf-16-le"):
        try:
            text = data.decode(encoding)
            if text:
                return text.replace("﻿", "")
        except UnicodeDecodeError:
            continue
    return data.decode("latin-1", "replace")


def parse_table_text(text: str) -> tuple[list[str], list[tuple[str, ...]]]:
    """用 csv.reader 整体解析，保留引号内换行；再按首列标记分离注释与数据。"""
    comments: list[str] = []
    rows: list[tuple[str, ...]] = []
    try:
        reader = csv.reader(io.StringIO(text.replace("\x00", "")))
        for raw in reader:
            cells = tuple(cell.strip() for cell in raw)
            if not any(cells):
                continue
            first = cells[0].strip()
            if first.startswith("#") or first.startswith("//"):
                comments.append(",".join(cells).strip())
            else:
                rows.append(cells)
        return comments, rows
    except csv.Error:
        pass
    for raw in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#") or line.startswith("//"):
            comments.append(line)
        else:
            rows.append(tuple(cell.strip() for cell in line.split(",")))
    return comments, rows


def read_table(reader: PakReader, key: str, pak_name: str, logical_path: str) -> TableText | None:
    hit = reader.read(pak_name, logical_path)
    if hit is None:
        return None
    path, payload = hit
    text = decode_text(payload)
    comments, rows = parse_table_text(text)
    return TableText(key, pak_name, path, text, tuple(comments), tuple(rows))


def to_int(value: str | None, default: int | None = None) -> int | None:
    if value is None:
        return default
    text = value.strip()
    if not text:
        return default
    try:
        return int(text, 10)
    except ValueError:
        return default


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def iter_batches(items: Iterable, size: int = 1000) -> Iterator[list]:
    batch: list = []
    for item in items:
        batch.append(item)
        if len(batch) >= size:
            yield batch
            batch = []
    if batch:
        yield batch
