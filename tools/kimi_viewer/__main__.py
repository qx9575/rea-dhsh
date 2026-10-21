"""入口：python -m kimi_viewer 或 python tools/kimi_viewer.py"""
from __future__ import annotations

import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))


def main() -> int:
    ap = argparse.ArgumentParser(description="kimi_viewer — PAK 资源查看器")
    ap.add_argument("--shdata", default=r"D:\software\dhsh\shdata",
                    help="PAK 所在目录")
    ap.add_argument("--inventory", default=os.path.join(ROOT, "pak_inventory.json"))
    ap.add_argument("--cache-dir", default=os.path.join(ROOT, "manifests"))
    args = ap.parse_args()

    from kimi_viewer.source import PakSource, load_inventory

    print(f"加载索引 {args.inventory} …", flush=True)
    entries, stale = load_inventory(args.inventory, args.cache_dir, args.shdata)
    print(f"{len(entries):,} 个条目", flush=True)
    if stale:
        print(f"注意：{len(stale)} 个 PAK 在索引生成后被更新，已实时重建："
              f"{', '.join(stale)}", flush=True)
    src = PakSource(entries, args.shdata)

    from kimi_viewer.app import run
    return run(src)


if __name__ == "__main__":
    sys.exit(main())
