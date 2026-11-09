"""art_pipeline CLI。

用法（仓库根目录）：
  python tools/art_pipeline/cli.py all
  python tools/art_pipeline/cli.py tables
  python tools/art_pipeline/cli.py inventory
  python tools/art_pipeline/cli.py careers
  python tools/art_pipeline/cli.py report
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from art_pipeline.common import (  # noqa: E402
    DEFAULT_DOCS,
    DEFAULT_INVENTORY,
    DEFAULT_OUT,
    DEFAULT_SHDATA,
)
from art_pipeline import careers, inventory, report, tables  # noqa: E402
from art_pipeline import scp_index, lua_index  # noqa: E402


def _paths(args) -> tuple[Path, Path, Path, Path | None]:
    shdata = Path(args.shdata)
    out = Path(args.out)
    docs = Path(args.docs)
    inv = None if args.rebuild_inventory else Path(args.inventory)
    return shdata, out, docs, inv


def cmd_tables(args) -> int:
    shdata, out, _docs, _inv = _paths(args)
    tables.build_tables(shdata, out)
    print(f"[tables] wrote {out / 'tables.json'} and {out / 'art_pipeline.sqlite'}")
    return 0


def cmd_inventory(args) -> int:
    shdata, out, _docs, inv = _paths(args)
    db = out / "art_pipeline.sqlite"
    if not db.exists():
        tables.build_tables(shdata, out)
    index = inventory.build_inventory(shdata, out, db, inv)
    print(f"[inventory] {len(index.entries)} leaves; {len(index.parts)} player parts; "
          f"{len(index.anomalies)} anomaly groups")
    return 0


def cmd_careers(args) -> int:
    shdata, out, _docs, inv = _paths(args)
    db = out / "art_pipeline.sqlite"
    loaded = tables.load_tables(tables.PakReader(shdata))
    index = inventory.build_inventory(shdata, out, db, inv)
    summary = careers.build_careers(loaded, index, db, out)
    print(f"[careers] {summary['presets']} presets; "
          f"missing={summary['missing_checks']}; "
          f"id_space_mismatch={summary['id_space_mismatch_checks']}; "
          f"stargod unresolved={summary['stargod']['unresolved']}")
    return 0


def cmd_scripts(args) -> int:
    shdata, out, _docs, _inv = _paths(args)
    db = out / "art_pipeline.sqlite"
    if not db.exists():
        tables.build_tables(shdata, out)
    scp = scp_index.build_scp_index(shdata, db, out, limit=args.scp_limit)
    lua = lua_index.build_lua_index(shdata, scp, db, out)
    print(f"[scripts] SCP files={len(scp.files)} windows={len(scp.windows)} "
          f"messages={len(scp.messages)} scripts={len(scp.scripts)}; "
          f"Lua units={len(lua.units)}")
    return 0


def cmd_report(args) -> int:
    shdata, out, docs, inv = _paths(args)
    db = out / "art_pipeline.sqlite"
    loaded = tables.load_tables(tables.PakReader(shdata))
    index = inventory.build_inventory(shdata, out, db, inv)
    summary = careers.build_careers(loaded, index, db, out)
    scp = scp_index.build_scp_index(shdata, db, out, limit=args.scp_limit)
    lua = lua_index.build_lua_index(shdata, scp, db, out)
    report.build_reports(loaded, index, summary, docs, scp, lua)
    print(f"[report] wrote {docs}")
    return 0


def cmd_all(args) -> int:
    shdata, out, docs, inv = _paths(args)
    out.mkdir(parents=True, exist_ok=True)
    loaded = tables.build_tables(shdata, out)
    print(f"[tables] {len(loaded)} tables")
    index = inventory.build_inventory(shdata, out, out / "art_pipeline.sqlite", inv)
    print(f"[inventory] {len(index.entries)} leaves; {len(index.parts)} player parts")
    summary = careers.build_careers(loaded, index, out / "art_pipeline.sqlite", out)
    print(f"[careers] {summary['presets']} presets; missing={summary['missing_checks']}; "
          f"id_space_mismatch={summary['id_space_mismatch_checks']}")
    scp = scp_index.build_scp_index(shdata, out / "art_pipeline.sqlite", out,
                                    limit=args.scp_limit)
    lua = lua_index.build_lua_index(shdata, scp, out / "art_pipeline.sqlite", out)
    print(f"[scripts] SCP files={len(scp.files)} windows={len(scp.windows)} "
          f"messages={len(scp.messages)}; Lua units={len(lua.units)}")
    report.build_reports(loaded, index, summary, docs, scp, lua)
    print(f"[report] {docs}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="纸娃娃/美术流程只读分析")
    parser.add_argument("command",
                        choices=["all", "tables", "inventory", "careers", "scripts", "report"])
    parser.add_argument("--shdata", default=str(DEFAULT_SHDATA))
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument("--docs", default=str(DEFAULT_DOCS))
    parser.add_argument("--inventory", default=str(DEFAULT_INVENTORY))
    parser.add_argument("--scp-limit", type=int, default=None,
                        help="只索引前 N 个 SCP（调试用）")
    parser.add_argument("--rebuild-inventory", action="store_true",
                        help="忽略 pak_inventory.json，直接扫描 PAK")
    args = parser.parse_args(argv)
    return {
        "all": cmd_all,
        "tables": cmd_tables,
        "inventory": cmd_inventory,
        "careers": cmd_careers,
        "scripts": cmd_scripts,
        "report": cmd_report,
    }[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())
