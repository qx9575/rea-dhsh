"""入口：python -m kimi_viewer 或 python tools/kimi_viewer.py"""
from __future__ import annotations

import argparse
import os
import sys
import time

# __main__.py 位于 <repo>/tools/kimi_viewer/ 下，仓库根 = 向上三层
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

# 索引与缓存默认放在「当前工作目录」下的专门目录，不找仓库根
DATA_DIR_NAME = "kimi_viewer_data"


def main() -> int:
    ap = argparse.ArgumentParser(description="kimi_viewer — PAK 资源查看器")
    ap.add_argument("--shdata", default=r"D:\software\dhsh\shdata",
                    help="PAK 所在目录")
    ap.add_argument("--inventory", default=None,
                    help=f"索引路径（默认 <当前目录>/{DATA_DIR_NAME}/pak_inventory.json）")
    ap.add_argument("--cache-dir", default=None,
                    help=f"缓存/配置目录（默认 <当前目录>/{DATA_DIR_NAME}）")
    args = ap.parse_args()

    data_dir = os.path.join(os.getcwd(), DATA_DIR_NAME)
    if args.inventory is None:
        args.inventory = os.path.join(data_dir, "pak_inventory.json")
    if args.cache_dir is None:
        args.cache_dir = data_dir

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
    src.imgcache_dir = os.path.join(args.cache_dir, "imgcache")

    print(f"[3/3] 打开主窗口 …", flush=True)
    from kimi_viewer.app import run

    code = run(src)
    print("窗口已关闭，退出。", flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
