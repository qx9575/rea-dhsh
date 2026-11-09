"""SCP 静态索引：窗口树、MESSAGE 绑定、#script 块。只读。"""
from __future__ import annotations

import hashlib
import json
import re
import sqlite3
from dataclasses import dataclass, field
from pathlib import Path

from art_pipeline.common import PakReader, decode_text, iter_batches, write_json
from dhsh.formats.scp import iter_nodes, parse_scp


@dataclass
class ScpWindow:
    scp_key: str
    path: str
    kind: str
    type: str
    name: str
    line: int
    props: list[tuple[str, str]] = field(default_factory=list)


@dataclass
class ScpMessage:
    scp_key: str
    window: str
    message: str
    arg: str
    code: str


@dataclass
class ScpScript:
    scp_key: str
    script_index: int
    text: str


@dataclass
class ScpFileInfo:
    scp_key: str
    pak: str
    path: str
    encoding: str
    windows: int
    includes: int
    warnings: int
    scripts: int


@dataclass
class ScpIndex:
    files: list[ScpFileInfo]
    windows: list[ScpWindow]
    messages: list[ScpMessage]
    scripts: list[ScpScript]

    def summary(self) -> dict:
        return {
            "files": len(self.files),
            "windows": len(self.windows),
            "messages": len(self.messages),
            "scripts": len(self.scripts),
            "files_with_scripts": sum(1 for f in self.files if f.scripts),
            "window_types": _counter(w.type for w in self.windows),
            "message_names": _counter(m.message for m in self.messages),
        }


def _counter(values) -> dict[str, int]:
    out: dict[str, int] = {}
    for value in values:
        out[value] = out.get(value, 0) + 1
    return dict(sorted(out.items(), key=lambda kv: (-kv[1], kv[0])))


_MESSAGE_RE = re.compile(
    r'^(?P<msg>\S+)\s+(?P<arg>\S+)\s+CODE\s+"?(?P<code>[^"]+)"?', re.IGNORECASE)


def _parse_message(value: str) -> tuple[str, str, str] | None:
    match = _MESSAGE_RE.match(value.strip())
    if not match:
        return None
    return match.group("msg"), match.group("arg"), match.group("code").strip()


def _scp_key(pak: str, path: str) -> str:
    return f"{pak}:{path}"


def build_scp_index(shdata: Path, db_path: Path, out_dir: Path,
                    limit: int | None = None) -> ScpIndex:
    conn = sqlite3.connect(db_path)
    rows = conn.execute(
        "SELECT pak, path FROM resource_files WHERE ext='.scp' ORDER BY pak, path").fetchall()
    conn.close()
    if limit:
        rows = rows[:limit]
    reader = PakReader(shdata)
    files: list[ScpFileInfo] = []
    windows: list[ScpWindow] = []
    messages: list[ScpMessage] = []
    scripts: list[ScpScript] = []
    for pak, path in rows:
        hit = reader.read(pak, path)
        if hit is None:
            continue
        try:
            scp = parse_scp(hit[1])
        except Exception as exc:  # noqa: BLE001
            print(f"[scp] skip {pak}:{path}: {exc}")
            continue
        key = _scp_key(pak, path)
        files.append(ScpFileInfo(key, pak, path, scp.encoding,
                                 len(scp.windows), len(scp.includes),
                                 len(scp.warnings), len(scp.scripts)))
        for index, text in enumerate(scp.scripts):
            scripts.append(ScpScript(key, index, text))
        for node in iter_nodes(scp.windows):
            props = list(node.props)
            name = ""
            for k, v in props:
                if k.upper() == "NAME":
                    name = v.strip().strip('"')
                    break
            windows.append(ScpWindow(key, path, node.kind, node.type, name,
                                     node.line, props))
            for k, v in props:
                if k.upper() == "MESSAGE":
                    parsed = _parse_message(v)
                    if parsed:
                        messages.append(ScpMessage(key, name, *parsed))

    _write_sqlite(files, windows, messages, scripts, db_path)
    index = ScpIndex(files, windows, messages, scripts)
    write_json(out_dir / "scp_index.json", {
        "summary": index.summary(),
        "files": [f.__dict__ for f in files],
        "messages": [m.__dict__ for m in messages],
        "scripts": [
            {"scp_key": s.scp_key, "script_index": s.script_index,
             "lines": s.text.count("\n") + 1,
             "sha256": hashlib.sha256(s.text.encode("utf-8")).hexdigest()}
            for s in scripts
        ],
    })
    return index


def _write_sqlite(files: list[ScpFileInfo], windows: list[ScpWindow],
                  messages: list[ScpMessage], scripts: list[ScpScript],
                  db_path: Path) -> None:
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    for table in ("scp_files", "scp_windows", "scp_messages", "scp_scripts"):
        cur.execute(f"DROP TABLE IF EXISTS {table}")
    cur.execute("""CREATE TABLE scp_files(
        scp_key TEXT PRIMARY KEY, pak TEXT, path TEXT, encoding TEXT,
        windows INTEGER, includes INTEGER, warnings INTEGER, scripts INTEGER)""")
    cur.execute("""CREATE TABLE scp_windows(
        scp_key TEXT, path TEXT, kind TEXT, type TEXT, name TEXT,
        line INTEGER, props TEXT)""")
    cur.execute("""CREATE TABLE scp_messages(
        scp_key TEXT, window TEXT, message TEXT, arg TEXT, code TEXT)""")
    cur.execute("""CREATE TABLE scp_scripts(
        scp_key TEXT, script_index INTEGER, lines INTEGER,
        sha256 TEXT, text TEXT, PRIMARY KEY(scp_key, script_index))""")
    cur.executemany("INSERT INTO scp_files VALUES (?,?,?,?,?,?,?,?)",
                    [(f.scp_key, f.pak, f.path, f.encoding, f.windows,
                      f.includes, f.warnings, f.scripts) for f in files])
    cur.executemany("INSERT INTO scp_windows VALUES (?,?,?,?,?,?,?)",
                    [(w.scp_key, w.path, w.kind, w.type, w.name, w.line,
                      json.dumps(w.props, ensure_ascii=False)) for w in windows])
    cur.executemany("INSERT INTO scp_messages VALUES (?,?,?,?,?)",
                    [(m.scp_key, m.window, m.message, m.arg, m.code) for m in messages])
    cur.executemany("INSERT INTO scp_scripts VALUES (?,?,?,?,?)",
                    [(s.scp_key, s.script_index, s.text.count("\n") + 1,
                      hashlib.sha256(s.text.encode("utf-8")).hexdigest(), s.text)
                     for s in scripts])
    conn.commit()
    conn.close()
