"""source 层：PAK 索引、mmap 随机读取、NZ 解码、LRU 缓存。

性能要点：
- pak_inventory.json（91MB）首次加载后编译为 pickle 缓存，二次启动秒开；
- 每个 PAK 用只读 mmap 打开，读条目 = 切片，零整包加载；
- NZ 解压结果按字节配额 LRU 缓存（默认 384MB），大量小文件来回切换不重复解压；
- 单个大文件只切所需区间，hex 预览可只取前若干 KB 不完整解压。
"""
from __future__ import annotations

import json
import mmap
import os
import pickle
import struct
import threading
from collections import OrderedDict
from dataclasses import dataclass

from dhsh import nz as dhnz

NZ_NAMES = {0: "原样", 0x0A: "LZW", 0x0B: "LZO"}
_SEP = "\x1f"


@dataclass(frozen=True, slots=True)
class Entry:
    pak: str
    path: str
    name: str
    ext: str
    offset: int  # PAK 内绝对偏移（NZ 头起始）
    size: int    # 含 NZ 头的存储大小
    nz: int      # 0/0x0A/0x0B，-1 = 非 NZ

    @property
    def key(self) -> str:
        return f"{self.pak}/{self.path}"


def _nz_type(head_hex: str) -> int:
    if len(head_hex) >= 8 and head_hex[:4] == "4e5a":
        return int(head_hex[6:8] + head_hex[4:6], 16)
    return -1


def _pak_sigs(shdata_dir: str, pak_names) -> tuple:
    sigs = []
    for p in sorted(pak_names):
        fp = os.path.join(shdata_dir, p)
        try:
            s = os.stat(fp)
            sigs.append((p, s.st_mtime, s.st_size))
        except OSError:
            sigs.append((p, None, None))
    return tuple(sigs)


def _head_nz(f, offset: int) -> int:
    """读条目头 4 字节判定 NZ 类型；非 NZ 返回 -1。"""
    f.seek(offset)
    b = f.read(4)
    if len(b) == 4 and b[:2] == b"NZ":
        return struct.unpack("<H", b[2:4])[0]
    return -1


def _probe_fresh(f, entries) -> bool:
    """抽样验证索引偏移是否仍然命中：不一致说明 PAK 在索引生成后被更新。

    等距取 5 个样本点（避开只查开头导致的漏判）。
    """
    candidates = [e for e in entries if e.nz >= 0 and e.size >= 12]
    if not candidates:
        return True
    n = len(candidates)
    for frac in (0.05, 0.25, 0.5, 0.75, 0.95):
        e = candidates[min(n - 1, int(n * frac))]
        if _head_nz(f, e.offset) != e.nz:
            return False
    return True


def _live_rows(pak: str, path: str) -> list[tuple]:
    """PAK 过期时用 dhsh.pak 实时解析（支持嵌套 PAKF3001）。"""
    from dhsh.pak import iter_pak

    rows = []
    with open(path, "rb") as f:
        for pe in iter_pak(path):
            ext = os.path.splitext(pe.name)[1].lower() or "<none>"
            rows.append((pak, pe.path, pe.name, ext, pe.offset, pe.size,
                         _head_nz(f, pe.offset)))
    return rows


def load_inventory(json_path: str, cache_dir: str,
                   shdata_dir: str | None = None) -> tuple[list[Entry], list[str]]:
    """加载全量索引；返回 (条目, 过期重建的 PAK 列表)。

    - 带 pickle 编译缓存，标签含索引文件与全部 PAK 的 mtime+size；
    - 每个 PAK 抽样校验偏移，过期（游戏更新过）的 PAK 自动实时重解析。
    """
    st = os.stat(json_path)
    os.makedirs(cache_dir, exist_ok=True)
    cache = os.path.join(cache_dir, "kimi_viewer_inv.cache")
    # 快路径：缓存记录自带 PAK 名单与签名，命中则不解析 91MB JSON
    try:
        with open(cache, "rb") as f:
            cjtag, pak_names, csigs, blob, stale = pickle.load(f)
        if cjtag == (st.st_mtime, st.st_size) and shdata_dir:
            if _pak_sigs(shdata_dir, pak_names) == csigs:
                entries = []
                for line in blob.decode().splitlines():
                    r = line.split(_SEP)
                    entries.append(Entry(r[0], r[1], r[2], r[3],
                                         int(r[4]), int(r[5]), int(r[6])))
                return entries, stale
    except Exception:
        pass

    with open(json_path, encoding="utf-8") as f:
        inv = json.load(f)
    sigs = _pak_sigs(shdata_dir, inv.keys()) if shdata_dir else None

    rows = []
    stale = []
    for pak, entries in inv.items():
        inv_rows = []
        for e in entries:
            name = e["name"]
            ext = os.path.splitext(name)[1].lower() or "<none>"
            inv_rows.append((pak, e["path"], name, ext, e["offset"], e["size"],
                             _nz_type(e.get("head", ""))))
        if shdata_dir:
            fp = os.path.join(shdata_dir, pak)
            try:
                f = open(fp, "rb")
            except OSError:
                continue  # PAK 缺失：跳过
            with f:
                probe_entries = [Entry(*r) for r in inv_rows]
                if not _probe_fresh(f, probe_entries):
                    inv_rows = _live_rows(pak, fp)
                    stale.append(pak)
        rows.extend(inv_rows)
    try:
        blob = "\n".join(_SEP.join(map(str, r)) for r in rows).encode()
        with open(cache, "wb") as f:
            pickle.dump(((st.st_mtime, st.st_size), sorted(inv.keys()),
                         sigs, blob, stale), f, protocol=5)
    except Exception:
        pass
    return [Entry(*r) for r in rows], stale


class ByteLRU:
    """按字节配额驱逐的 LRU（线程安全）。"""

    def __init__(self, max_bytes: int = 384 * 1024 * 1024):
        self.max_bytes = max_bytes
        self._d: OrderedDict[str, tuple[bytes, object]] = OrderedDict()
        self._bytes = 0
        self._lock = threading.Lock()
        self.hits = 0
        self.misses = 0

    def get(self, key: str):
        with self._lock:
            v = self._d.get(key)
            if v is None:
                self.misses += 1
                return None
            self._d.move_to_end(key)
            self.hits += 1
            return v

    def put(self, key: str, value: tuple[bytes, object]):
        n = len(value[0])
        if n > self.max_bytes:
            return  # 单条超过配额则不缓存
        with self._lock:
            old = self._d.pop(key, None)
            if old is not None:
                self._bytes -= len(old[0])
            while self._bytes + n > self.max_bytes and self._d:
                _, (b, _) = self._d.popitem(last=False)
                self._bytes -= len(b)
            self._d[key] = value
            self._bytes += n

    def stats(self) -> str:
        with self._lock:
            return f"缓存 {len(self._d)} 条 / {self._bytes / 1048576:.0f}MB，命中 {self.hits} 未命中 {self.misses}"


class PakSource:
    """PAK 随机访问源：mmap + NZ 解码 + LRU。"""

    def __init__(self, entries: list[Entry], shdata_dir: str,
                 cache_bytes: int = 384 * 1024 * 1024):
        self.entries = entries
        self.shdata_dir = shdata_dir
        self.cache = ByteLRU(cache_bytes)
        self._files: dict[str, object] = {}
        self._mmaps: dict[str, mmap.mmap] = {}
        # pak -> dir_lower -> {name_lower: Entry}（兄弟文件/调色板查找）
        self._by_pak_dir: dict[str, dict[str, dict[str, Entry]]] = {}
        for e in entries:
            d = e.path.rsplit("/", 1)[0].lower() if "/" in e.path else ""
            slot = self._by_pak_dir.setdefault(e.pak, {}).setdefault(d, {})
            slot.setdefault(e.name.lower(), e)

    # ---- 底层读取 ----
    def _mmap(self, pak: str) -> mmap.mmap:
        m = self._mmaps.get(pak)
        if m is None:
            f = open(os.path.join(self.shdata_dir, pak), "rb")
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            self._files[pak] = f
            self._mmaps[pak] = m
        return m

    def raw_entry(self, e: Entry, limit: int | None = None) -> bytes:
        """存储字节（含 NZ 头）；limit 截断用于 hex 预览。"""
        m = self._mmap(e.pak)
        end = e.offset + (min(e.size, limit) if limit else e.size)
        return m[e.offset:end]

    def decode_entry(self, e: Entry) -> tuple[bytes, str]:
        """返回 (逻辑 payload, NZ 描述)。带 LRU。"""
        hit = self.cache.get(e.key)
        if hit is not None:
            return hit
        raw = self.raw_entry(e)
        hdr = dhnz.parse_header(raw)
        if hdr is None:
            out = (raw, "非 NZ")
        elif hdr.stored:
            out = (raw[dhnz.HEADER_SIZE:], f"NZ 原样 {hdr.size2}B")
        else:
            payload = dhnz.decompress(hdr.kind, raw[dhnz.HEADER_SIZE:], hdr.size1)
            ratio = hdr.size2 / hdr.size1 if hdr.size1 else 0
            out = (payload, f"NZ {NZ_NAMES.get(hdr.kind, hex(hdr.kind))} "
                            f"{hdr.size1}B←{hdr.size2}B ({ratio:.0%})")
        self.cache.put(e.key, out)
        return out

    def find_sibling(self, e: Entry, name: str) -> Entry | None:
        """同 PAK 同目录找兄弟文件。"""
        d = e.path.rsplit("/", 1)[0].lower() if "/" in e.path else ""
        return self._by_pak_dir.get(e.pak, {}).get(d, {}).get(name.lower())

    def resolve_clrtb(self, e: Entry, name: str = "clrtb.cti") -> Entry | None:
        """沿逻辑目录向上找调色板：同 PAK 优先，逐级到根，再跨 PAK。

        与 export_apg/check_apg_cti 的解析规则一致。
        """
        d = e.path.rsplit("/", 1)[0].lower() if "/" in e.path else ""
        dirs = [d]
        while d:
            d = d.rsplit("/", 1)[0] if "/" in d else ""
            dirs.append(d)
        paks = [e.pak] + [p for p in self._by_pak_dir if p != e.pak]
        for pak in paks:
            pd = self._by_pak_dir.get(pak, {})
            for d in dirs:
                hit = pd.get(d, {}).get(name.lower())
                if hit is not None:
                    return hit
        return None

    def close(self):
        for m in self._mmaps.values():
            m.close()
        for f in self._files.values():
            f.close()


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f}{unit}" if unit != "B" else f"{int(n)}B"
        n /= 1024
    return f"{n:.1f}GB"


def nz_label(code: int) -> str:
    return NZ_NAMES.get(code, "非NZ") if code >= 0 else "非NZ"
