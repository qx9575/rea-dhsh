"""Lua/SCP 脚本静态索引：FSLua API、窗口名、消息回调、CallScProcess。

不执行 Lua；只做正则级静态提取。
"""
from __future__ import annotations

import hashlib
import json
import re
import sqlite3
from dataclasses import dataclass
from pathlib import Path

from art_pipeline.common import PakReader, decode_text, write_json
from art_pipeline.scp_index import ScpIndex


@dataclass
class LuaUnit:
    unit_id: str
    source_type: str
    pak: str
    path: str
    script_index: int
    lines: int
    sha256: str
    text: str


@dataclass
class LuaIndex:
    units: list[LuaUnit]
    apis: list[tuple[str, str, int]]          # unit_id, api, count
    windows: list[tuple[str, str, str]]       # unit_id, api, name
    calls: list[tuple[str, str, str]]         # unit_id, process, value
    messages: list[tuple[str, str, str]]      # unit_id, api, name
    requires: list[tuple[str, str]]           # unit_id, module
    installs: list[tuple[str, str]]           # unit_id, module
    strings: list[tuple[str, str]]            # unit_id, value

    def summary(self) -> dict:
        api_counts: dict[str, int] = {}
        for _unit, api, count in self.apis:
            api_counts[api] = api_counts.get(api, 0) + count
        window_counts: dict[str, int] = {}
        for _unit, _api, name in self.windows:
            window_counts[name] = window_counts.get(name, 0) + 1
        call_counts: dict[str, int] = {}
        for _unit, process, _value in self.calls:
            call_counts[process] = call_counts.get(process, 0) + 1
        install_counts: dict[str, int] = {}
        for _unit, module in self.installs:
            install_counts[module] = install_counts.get(module, 0) + 1
        return {
            "units": len(self.units),
            "lua_units": sum(1 for u in self.units if u.source_type == "lua"),
            "scp_units": sum(1 for u in self.units if u.source_type == "scp"),
            "apis": dict(sorted(api_counts.items(), key=lambda kv: (-kv[1], kv[0]))),
            "windows": dict(sorted(window_counts.items(), key=lambda kv: (-kv[1], kv[0]))),
            "callsc": dict(sorted(call_counts.items(), key=lambda kv: (-kv[1], kv[0]))),
            "installs": dict(sorted(install_counts.items(), key=lambda kv: (-kv[1], kv[0]))),
            "messages": _message_counts(self.messages),
            "requires": sorted({m for _u, m in self.requires}),
        }


def _message_counts(items: list[tuple[str, str, str]]) -> dict[str, int]:
    out: dict[str, int] = {}
    for _unit, _api, name in items:
        out[name] = out.get(name, 0) + 1
    return dict(sorted(out.items(), key=lambda kv: (-kv[1], kv[0])))


API_RE = re.compile(r'\b(FSLuaSystem|FSLuaWndScript|FSLuaPluginsMng)\.([A-Za-z_]\w*)')
WINDOW_RE = re.compile(
    r'\bFSLuaSystem\.(GetRootWindow|SearchRootWindow|CreateRootWindow|CreateRootModal)'
    r'\s*\(\s*["\']([^"\']+)')
CALLSC_RE = re.compile(
    r':\s*CallScProcess\s*\(\s*["\']([^"\']+)["\']\s*(?:,\s*([^)]*))?\)')
REGMSG_RE = re.compile(r'\bFSLuaWndScript\.RegMsgCall\s*\(\s*["\']([^"\']+)')
REGFUNC_RE = re.compile(r'\bFSLuaSystem\.RegFunction\s*\(\s*["\']([^"\']+)')
REQUIRE_RE = re.compile(r'\b(?:require|dofile|loadfile)\s*\(?\s*["\']([^"\']+)')
INSTALL_RE = re.compile(r'\bFSLuaInstall\s*\(\s*["\']([^"\']+)')
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"|\'((?:[^\'\\]|\\.)*)\'')


def _unit_id(source_type: str, pak: str, path: str, index: int) -> str:
    return f"{source_type}:{pak}:{path}#{index}"


def _add_unit(unit: LuaUnit, apis, windows, calls, messages, requires,
              installs, strings) -> None:
    for match in API_RE.finditer(unit.text):
        apis.append((unit.unit_id, f"{match.group(1)}.{match.group(2)}", 1))
    for match in WINDOW_RE.finditer(unit.text):
        windows.append((unit.unit_id, f"FSLuaSystem.{match.group(1)}", match.group(2)))
    for match in CALLSC_RE.finditer(unit.text):
        calls.append((unit.unit_id, match.group(1), (match.group(2) or "").strip()))
    for match in REGMSG_RE.finditer(unit.text):
        messages.append((unit.unit_id, "RegMsgCall", match.group(1)))
    for match in REGFUNC_RE.finditer(unit.text):
        messages.append((unit.unit_id, "RegFunction", match.group(1)))
    for match in REQUIRE_RE.finditer(unit.text):
        requires.append((unit.unit_id, match.group(1)))
    for match in INSTALL_RE.finditer(unit.text):
        installs.append((unit.unit_id, match.group(1)))
    seen: set[str] = set()
    for match in STRING_RE.finditer(unit.text):
        value = match.group(1) if match.group(1) is not None else match.group(2)
        if value and value not in seen and len(value) <= 200:
            seen.add(value)
            strings.append((unit.unit_id, value))


def build_lua_index(shdata: Path, scp: ScpIndex, db_path: Path,
                    out_dir: Path) -> LuaIndex:
    reader = PakReader(shdata)
    units: list[LuaUnit] = []
    apis: list[tuple[str, str, int]] = []
    windows: list[tuple[str, str, str]] = []
    calls: list[tuple[str, str, str]] = []
    messages: list[tuple[str, str, str]] = []
    requires: list[tuple[str, str]] = []
    installs: list[tuple[str, str]] = []
    strings: list[tuple[str, str]] = []

    conn = sqlite3.connect(db_path)
    lua_rows = conn.execute(
        "SELECT pak, path FROM resource_files WHERE ext='.lua' ORDER BY pak, path").fetchall()
    conn.close()
    for pak, path in lua_rows:
        hit = reader.read(pak, path)
        if hit is None:
            continue
        text = decode_text(hit[1])
        unit = LuaUnit(_unit_id("lua", pak, path, 0), "lua", pak, path, 0,
                       text.count("\n") + 1,
                       hashlib.sha256(text.encode("utf-8")).hexdigest(), text)
        units.append(unit)
        _add_unit(unit, apis, windows, calls, messages, requires, installs, strings)

    for script in scp.scripts:
        unit = LuaUnit(_unit_id("scp", script.scp_key, "", script.script_index),
                       "scp", script.scp_key.split(":", 1)[0],
                       script.scp_key, script.script_index,
                       script.text.count("\n") + 1,
                       hashlib.sha256(script.text.encode("utf-8")).hexdigest(),
                       script.text)
        units.append(unit)
        _add_unit(unit, apis, windows, calls, messages, requires, installs, strings)

    # 合并重复 API 计数
    merged_apis: dict[tuple[str, str], int] = {}
    for unit_id, api, count in apis:
        merged_apis[(unit_id, api)] = merged_apis.get((unit_id, api), 0) + count
    apis = [(u, a, c) for (u, a), c in merged_apis.items()]

    index = LuaIndex(units, apis, windows, calls, messages, requires, installs, strings)
    _write_sqlite(index, db_path)
    write_json(out_dir / "lua_index.json", index.summary())
    return index


def _write_sqlite(index: LuaIndex, db_path: Path) -> None:
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    for table in ("lua_units", "lua_apis", "lua_windows", "lua_calls",
                  "lua_messages", "lua_requires", "lua_installs", "lua_strings"):
        cur.execute(f"DROP TABLE IF EXISTS {table}")
    cur.execute("""CREATE TABLE lua_units(
        unit_id TEXT PRIMARY KEY, source_type TEXT, pak TEXT, path TEXT,
        script_index INTEGER, lines INTEGER, sha256 TEXT, text TEXT)""")
    cur.execute("""CREATE TABLE lua_apis(
        unit_id TEXT, api TEXT, count INTEGER)""")
    cur.execute("""CREATE TABLE lua_windows(
        unit_id TEXT, api TEXT, name TEXT)""")
    cur.execute("""CREATE TABLE lua_calls(
        unit_id TEXT, process TEXT, value TEXT)""")
    cur.execute("""CREATE TABLE lua_messages(
        unit_id TEXT, api TEXT, name TEXT)""")
    cur.execute("""CREATE TABLE lua_requires(
        unit_id TEXT, module TEXT)""")
    cur.execute("""CREATE TABLE lua_installs(
        unit_id TEXT, module TEXT)""")
    cur.execute("""CREATE TABLE lua_strings(
        unit_id TEXT, value TEXT)""")
    cur.executemany("INSERT INTO lua_units VALUES (?,?,?,?,?,?,?,?)",
                    [(u.unit_id, u.source_type, u.pak, u.path, u.script_index,
                      u.lines, u.sha256, u.text) for u in index.units])
    cur.executemany("INSERT INTO lua_apis VALUES (?,?,?)", index.apis)
    cur.executemany("INSERT INTO lua_windows VALUES (?,?,?)", index.windows)
    cur.executemany("INSERT INTO lua_calls VALUES (?,?,?)", index.calls)
    cur.executemany("INSERT INTO lua_messages VALUES (?,?,?)", index.messages)
    cur.executemany("INSERT INTO lua_requires VALUES (?,?)", index.requires)
    cur.executemany("INSERT INTO lua_installs VALUES (?,?)", index.installs)
    cur.executemany("INSERT INTO lua_strings VALUES (?,?)", index.strings)
    conn.commit()
    conn.close()
