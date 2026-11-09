"""版本基线快照与差异。

用法：
  python tools/art_pipeline/baseline.py snapshot --shdata D:/software/dhsh/shdata
  python tools/art_pipeline/baseline.py snapshot --shdata D:/software/dhsh2/shdata
  python tools/art_pipeline/baseline.py diff \
      --old manifests/baselines/<old>/baseline.json \
      --new manifests/baselines/<new>/baseline.json

只读 shdata；只写 manifests/baselines/ 与 docs/baselines/。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from dhsh import pak as dpak  # noqa: E402
from art_pipeline.common import PakReader, DEFAULT_DOCS  # noqa: E402
from art_pipeline import tables  # noqa: E402

DEFAULT_BASELINES = REPO_ROOT / "manifests" / "baselines"


def sha256_file(path: Path, chunk: int = 8 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            block = fh.read(chunk)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def read_text_safe(path: Path) -> str:
    try:
        return path.read_text(encoding="gb18030", errors="replace")[:20000]
    except OSError:
        return ""


def table_hashes(shdata: Path) -> dict:
    reader = PakReader(shdata)
    loaded = tables.load_tables(reader)
    out = {}
    for key, table in loaded.items():
        out[key] = {
            "pak": table.pak,
            "path": table.path,
            "rows": len(table.rows),
            "sha256": hashlib.sha256(table.text.encode("utf-8")).hexdigest(),
        }
    return out


def snapshot(shdata: Path, out_dir: Path | None = None,
             hash_paks: bool = True) -> dict:
    game_exe = shdata / "Game.exe"
    tag = "unknown"
    game_info = {}
    if game_exe.is_file():
        game_sha = sha256_file(game_exe)
        tag = game_sha[:12] if game_sha else "game"
        game_info = {
            "path": str(game_exe),
            "sha256": game_sha,
            "size": game_exe.stat().st_size,
            "mtime": datetime.fromtimestamp(game_exe.stat().st_mtime).isoformat(),
        }
    version = {
        name: read_text_safe(shdata / name)
        for name in ("conf.ini", "ver.csv", "save/update.txt")
    }
    pak_rows = []
    total_entries = 0
    for name in sorted(os.listdir(shdata)):
        if not name.lower().endswith(".pak"):
            continue
        path = shdata / name
        if not path.is_file():
            continue
        entries = 0
        data_bytes = 0
        try:
            for pe in dpak.iter_pak(str(path)):
                entries += 1
                data_bytes += pe.size
        except Exception as exc:  # noqa: BLE001
            print(f"[baseline] WARN {name}: {exc}")
        total_entries += entries
        pak_rows.append({
            "name": name,
            "size": path.stat().st_size,
            "mtime": datetime.fromtimestamp(path.stat().st_mtime).isoformat(),
            "sha256": sha256_file(path) if hash_paks else "",
            "entries": entries,
            "data_bytes": data_bytes,
        })
    result = {
        "tag": tag,
        "shdata": str(shdata),
        "created_at": datetime.now().isoformat(),
        "game_exe": game_info,
        "version": version,
        "paks": pak_rows,
        "total_entries": total_entries,
        "tables": table_hashes(shdata),
    }
    if out_dir is None:
        out_dir = DEFAULT_BASELINES / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "baseline.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    return result


def _index_by_name(rows: list[dict], key: str = "name") -> dict:
    return {row[key]: row for row in rows}


def diff(old: dict, new: dict) -> dict:
    old_paks = _index_by_name(old.get("paks", []))
    new_paks = _index_by_name(new.get("paks", []))
    pak_changes = []
    for name in sorted(set(old_paks) | set(new_paks)):
        a = old_paks.get(name)
        b = new_paks.get(name)
        if a is None:
            pak_changes.append({"name": name, "status": "added", "new": b})
        elif b is None:
            pak_changes.append({"name": name, "status": "removed", "old": a})
        elif (a.get("size") != b.get("size")
              or a.get("entries") != b.get("entries")
              or (a.get("sha256") and b.get("sha256")
                  and a.get("sha256") != b.get("sha256"))):
            pak_changes.append({
                "name": name,
                "status": "changed",
                "old": a,
                "new": b,
                "size_delta": (b.get("size") or 0) - (a.get("size") or 0),
                "entry_delta": (b.get("entries") or 0) - (a.get("entries") or 0),
            })
    old_tables = old.get("tables", {})
    new_tables = new.get("tables", {})
    table_changes = []
    for key in sorted(set(old_tables) | set(new_tables)):
        a = old_tables.get(key)
        b = new_tables.get(key)
        if a is None:
            table_changes.append({"key": key, "status": "added", "new": b})
        elif b is None:
            table_changes.append({"key": key, "status": "removed", "old": a})
        elif (a.get("sha256") != b.get("sha256")
              or a.get("rows") != b.get("rows")):
            table_changes.append({
                "key": key,
                "status": "changed",
                "old": a,
                "new": b,
                "row_delta": (b.get("rows") or 0) - (a.get("rows") or 0),
            })
    return {
        "old_tag": old.get("tag"),
        "new_tag": new.get("tag"),
        "game_exe": {
            "old": old.get("game_exe", {}),
            "new": new.get("game_exe", {}),
            "changed": old.get("game_exe", {}).get("sha256")
            != new.get("game_exe", {}).get("sha256"),
        },
        "total_entries": {
            "old": old.get("total_entries"),
            "new": new.get("total_entries"),
            "delta": (new.get("total_entries") or 0) - (old.get("total_entries") or 0),
        },
        "paks": pak_changes,
        "tables": table_changes,
    }


def render_markdown(result: dict, out_path: Path) -> None:
    lines = [
        "# 版本基线差异",
        "",
        f"- 旧基线：`{result.get('old_tag')}`",
        f"- 新基线：`{result.get('new_tag')}`",
        "",
        "## 1. Game.exe",
        "",
        f"- 旧 SHA-256：`{result['game_exe']['old'].get('sha256', '')}`",
        f"- 新 SHA-256：`{result['game_exe']['new'].get('sha256', '')}`",
        f"- 是否变化：{'是' if result['game_exe']['changed'] else '否'}",
        "",
        "## 2. 资源条目总数",
        "",
        f"- 旧：{result['total_entries']['old']}",
        f"- 新：{result['total_entries']['new']}",
        f"- 变化：{result['total_entries']['delta']:+d}",
        "",
        "## 3. PAK 变化",
        "",
        "| PAK | 状态 | 旧大小 | 新大小 | 大小变化 | 旧条目 | 新条目 | 条目变化 |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for change in result["paks"]:
        old = change.get("old") or {}
        new = change.get("new") or {}
        lines.append(
            f"| {change['name']} | {change['status']} | {old.get('size', '')} | "
            f"{new.get('size', '')} | {change.get('size_delta', '')} | "
            f"{old.get('entries', '')} | {new.get('entries', '')} | "
            f"{change.get('entry_delta', '')} |")
    lines += [
        "",
        "## 4. 表变化",
        "",
        "| 表 | 状态 | 旧行数 | 新行数 | 行变化 |",
        "|---|---|---:|---:|---:|",
    ]
    for change in result["tables"]:
        old = change.get("old") or {}
        new = change.get("new") or {}
        lines.append(
            f"| {change['key']} | {change['status']} | {old.get('rows', '')} | "
            f"{new.get('rows', '')} | {change.get('row_delta', '')} |")
    lines.append("")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="版本基线快照/差异")
    sub = parser.add_subparsers(dest="command", required=True)
    snap = sub.add_parser("snapshot")
    snap.add_argument("--shdata", required=True)
    snap.add_argument("--out", default="")
    snap.add_argument("--no-hash-paks", action="store_true")
    diffp = sub.add_parser("diff")
    diffp.add_argument("--old", required=True)
    diffp.add_argument("--new", required=True)
    diffp.add_argument("--out", default="")
    diffp.add_argument("--docs", default=str(REPO_ROOT / "docs" / "baselines"))
    args = parser.parse_args(argv)

    if args.command == "snapshot":
        out = Path(args.out) if args.out else None
        result = snapshot(Path(args.shdata), out, not args.no_hash_paks)
        print(f"[baseline] {result['tag']}: {len(result['paks'])} paks, "
              f"{result['total_entries']} entries, "
              f"{len(result['tables'])} tables")
        return 0

    old = json.loads(Path(args.old).read_text(encoding="utf-8"))
    new = json.loads(Path(args.new).read_text(encoding="utf-8"))
    result = diff(old, new)
    out = Path(args.out) if args.out else (
        REPO_ROOT / "manifests" / "baselines"
        / f"diff-{result['old_tag']}-vs-{result['new_tag']}.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    doc = Path(args.docs) / f"{result['old_tag']}-vs-{result['new_tag']}.md"
    render_markdown(result, doc)
    print(f"[diff] {out}")
    print(f"[diff] {doc}")
    print(f"[diff] game.exe changed={result['game_exe']['changed']}; "
          f"pak_changes={len(result['paks'])}; table_changes={len(result['tables'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
