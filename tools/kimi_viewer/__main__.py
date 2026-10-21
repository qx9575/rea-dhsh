"""入口：python -m kimi_viewer 或 python tools/kimi_viewer.py"""
from __future__ import annotations

import argparse
import os
import sys
import time

# __main__.py 位于 <repo>/tools/kimi_viewer/ 下，仓库根 = 向上三层
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))


def _find_inventory() -> str:
    """默认索引：仓库根 pak_inventory.json；向上兜底搜索防目录结构变化。"""
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(5):
        cand = os.path.join(d, "pak_inventory.json")
        if os.path.exists(cand):
            return cand
        d = os.path.dirname(d)
    return os.path.join(ROOT, "pak_inventory.json")


def main() -> int:
    ap = argparse.ArgumentParser(description="kimi_viewer — PAK 资源查看器")
    ap.add_argument("--shdata", default=r"D:\software\dhsh\shdata",
                    help="PAK 所在目录")
    ap.add_argument("--inventory", default=_find_inventory())
    ap.add_argument("--cache-dir", default=os.path.join(ROOT, "manifests"))
    args = ap.parse_args()

    from kimi_viewer.qtcompat import QT_BIND
    from kimi_viewer.source import PakSource, load_inventory

    print(f"[1/3] Qt 绑定：{QT_BIND}", flush=True)
    if not os.path.exists(args.inventory):
        print(f"[2/3] 索引 {args.inventory} 不存在：将直接扫描 "
              f"{args.shdata} 下的 .pak 全量重建（一次性，约 1 分钟），"
              f"并重新生成该索引文件。", flush=True)
    else:
        print(f"[2/3] 加载索引 {args.inventory} …（首次或 PAK 更新后需重建，约 30s；"
              f"缓存命中约 6s）", flush=True)
    t0 = time.perf_counter()
    entries, stale = load_inventory(args.inventory, args.cache_dir, args.shdata)
    print(f"      索引就绪：{len(entries):,} 个条目，耗时 {time.perf_counter() - t0:.1f}s",
          flush=True)
    if stale:
        print(f"      注意：{len(stale)} 个 PAK 在索引生成后被更新，已实时重建："
              f"{', '.join(stale)}", flush=True)
    src = PakSource(entries, args.shdata)
    src.config_path = os.path.join(args.cache_dir, "kimi_viewer_config.json")

    print(f"[3/3] 打开主窗口 …", flush=True)
    from kimi_viewer.app import run

    code = run(src)
    print("窗口已关闭，退出。", flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
