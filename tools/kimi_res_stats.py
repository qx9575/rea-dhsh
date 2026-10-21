#!/usr/bin/env python3
"""kimi_res_stats.py — 新水浒Q传资源统计工具

统计 pak_inventory.json 中全部 PAK 资源的数量与分布：

  1. 总览：PAK 数、文件数、逻辑/存储体积
  2. 按格式族（动画/图像/地图/脚本/配置……）：数量、字节、占比
  3. 按扩展名
  4. 按 PAK × 格式族 矩阵
  5. 按顶层目录（每个 PAK 内 path 的第一段）
  6. NZ 压缩类型分布（0x00 原样 / 0x0A LZW / 0x0B LZO / 非 NZ）
  7. 提取覆盖：对照 manifests/*.csv 统计已解包文件数
  8. （可选）散目录扫描：sound/ font/ animation/ update/skin/

用法：
  python tools/kimi_res_stats.py                      # 控制台摘要 + 写 JSON/MD
  python tools/kimi_res_stats.py --json out.json --md out.md
  python tools/kimi_res_stats.py --loose D:/software/dhsh/shdata
  python tools/kimi_res_stats.py --top-dirs 15 --no-write
"""
import argparse
import collections
import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INV_PATH = os.path.join(ROOT, "pak_inventory.json")
MANIFEST_DIR = os.path.join(ROOT, "manifests")

FAMILIES = {
    ".apg": "动画帧组(APG)",
    ".a16": "专有图像(A16/PG2)",
    ".pg2": "专有图像(A16/PG2)",
    ".cti": "染色表(CTI)",
    ".tga": "标准图像",
    ".jpg": "标准图像",
    ".bmp": "标准图像",
    ".png": "标准图像",
    ".psd": "标准图像",
    ".scp": "脚本(SCP/Lua)",
    ".lua": "脚本(SCP/Lua)",
    ".map": "地图数据",
    ".man": "地图数据",
    ".mlk": "地图数据",
    ".msk": "地图数据",
    ".cel": "地图数据",
    ".core": "地图数据",
    ".csv": "配置/文本",
    ".ini": "配置/文本",
    ".txt": "配置/文本",
    ".dat": "数据块(DAT)",
    ".db": "数据块(DAT)",
    ".gal": "其他",
    ".zip": "其他",
    "<none>": "其他",
}
NZ_NAMES = {0x00: "NZ-0x00 原样", 0x0A: "NZ-0x0A LZW", 0x0B: "NZ-0x0B LZO"}

COVERAGE_CSVS = [
    "easy-extract.csv",
    "images-proprietary.csv",
    "images-standard.csv",
    "misc.csv",
    "samples-apg.csv",
    "samples-compressed.csv",
]


def nz_type_of(head_hex):
    """head 为前 12 字节 hex；'4e5a' 开头则按小端取 u16 type。"""
    if len(head_hex) >= 8 and head_hex[:4] == "4e5a":
        return int(head_hex[6:8] + head_hex[4:6], 16)
    return -1  # 非 NZ


def family_of(ext):
    return FAMILIES.get(ext, "其他")


def human(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.1f}{unit}" if unit != "B" else f"{n}B"
        n /= 1024


def load_inventory(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def scan_loose(root):
    """扫描 PAK 之外的散目录（sound/font/animation/update/skin 等）。"""
    out = {}
    for top in sorted(os.listdir(root)):
        d = os.path.join(root, top)
        if not os.path.isdir(d):
            continue
        ext_counter = collections.Counter()
        nbytes = 0
        nfiles = 0
        for _, _, files in os.walk(d):
            for fn in files:
                ext = os.path.splitext(fn)[1].lower() or "<none>"
                ext_counter[ext] += 1
                nfiles += 1
                try:
                    nbytes += os.path.getsize(os.path.join(d, fn))
                except OSError:
                    pass
        if nfiles:
            out[top] = {"files": nfiles, "bytes": nbytes, "ext": dict(ext_counter)}
    return out


def main():
    ap = argparse.ArgumentParser(description="新水浒Q传资源统计")
    ap.add_argument("--inventory", default=INV_PATH)
    ap.add_argument("--json", default=os.path.join(MANIFEST_DIR, "res-stats.json"))
    ap.add_argument("--md", default=os.path.join(ROOT, "docs", "res-stats.md"))
    ap.add_argument("--loose", metavar="SHDATA_DIR", help="额外扫描散目录（如 D:/software/dhsh/shdata）")
    ap.add_argument("--top-dirs", type=int, default=10, help="每 PAK 显示的顶层目录数")
    ap.add_argument("--no-write", action="store_true", help="只打印，不写文件")
    args = ap.parse_args()

    inv = load_inventory(args.inventory)

    # ---- 汇总 ----
    fam_cnt = collections.Counter()
    fam_bytes = collections.Counter()
    ext_cnt = collections.Counter()
    ext_bytes = collections.Counter()
    pak_fam = {}           # pak -> Counter(family)
    pak_bytes = collections.Counter()
    pak_cnt = collections.Counter()
    pak_dirs = {}          # pak -> Counter(topdir)
    nz_cnt = collections.Counter()
    nz_bytes = collections.Counter()
    total_files = 0
    total_bytes = 0

    for pak, entries in inv.items():
        pf = collections.Counter()
        pd = collections.Counter()
        for e in entries:
            ext = os.path.splitext(e["name"])[1].lower() or "<none>"
            fam = family_of(ext)
            size = e["size"]
            nz = nz_type_of(e.get("head", ""))
            top = e["path"].split("/", 1)[0] if "/" in e["path"] else "<root>"

            fam_cnt[fam] += 1
            fam_bytes[fam] += size
            ext_cnt[ext] += 1
            ext_bytes[ext] += size
            pf[fam] += 1
            pd[top] += 1
            pak_bytes[pak] += size
            pak_cnt[pak] += 1
            nz_cnt[nz] += 1
            nz_bytes[nz] += size
            total_files += 1
            total_bytes += size
        pak_fam[pak] = pf
        pak_dirs[pak] = pd

    # ---- 提取覆盖 ----
    extracted = set()
    for name in COVERAGE_CSVS:
        p = os.path.join(MANIFEST_DIR, name)
        if not os.path.exists(p):
            continue
        with open(p, encoding="utf-8", newline="") as f:
            for row in csv.DictReader(f):
                extracted.add((row["pak"], row["path"]))
    inv_keys = {(pak, e["path"]) for pak, entries in inv.items() for e in entries}
    covered = inv_keys & extracted
    cov_fam = collections.Counter()
    for pak, path in covered:
        ext = os.path.splitext(path)[1].lower() or "<none>"
        cov_fam[family_of(ext)] += 1

    result = {
        "overview": {
            "paks": len(inv),
            "files": total_files,
            "bytes": total_bytes,
            "bytes_human": human(total_bytes),
        },
        "by_family": [
            {"family": k, "files": fam_cnt[k], "bytes": fam_bytes[k],
             "bytes_human": human(fam_bytes[k]),
             "pct_files": round(fam_cnt[k] * 100 / total_files, 2),
             "pct_bytes": round(fam_bytes[k] * 100 / total_bytes, 2)}
            for k, _ in fam_cnt.most_common()
        ],
        "by_ext": [
            {"ext": k, "files": ext_cnt[k], "bytes_human": human(ext_bytes[k])}
            for k, _ in ext_cnt.most_common()
        ],
        "nz_types": [
            {"type": NZ_NAMES.get(k, f"NZ-0x{k:02X} 未知") if k >= 0 else "非 NZ 头",
             "files": nz_cnt[k], "bytes_human": human(nz_bytes[k]),
             "pct_files": round(nz_cnt[k] * 100 / total_files, 2)}
            for k, _ in nz_cnt.most_common()
        ],
        "by_pak": [
            {"pak": p, "files": pak_cnt[p], "bytes_human": human(pak_bytes[p]),
             "top_family": pak_fam[p].most_common(1)[0][0],
             "families": dict(pak_fam[p])}
            for p in sorted(inv)
        ],
        "top_dirs": {p: pak_dirs[p].most_common(args.top_dirs) for p in sorted(inv)},
        "extraction_coverage": {
            "manifest_files": len(covered),
            "pct": round(len(covered) * 100 / total_files, 2),
            "by_family": dict(cov_fam),
        },
    }

    if args.loose:
        result["loose_dirs"] = scan_loose(args.loose)

    # ---- 控制台摘要 ----
    o = result["overview"]
    print(f"== 总览 ==  {o['paks']} 个 PAK / {o['files']:,} 个文件 / {o['bytes_human']}（PAK 内存储体积）")
    print("\n== 按格式族 ==")
    print(f"{'格式族':<16s} {'文件数':>9s} {'占比':>7s} {'体积':>10s} {'体积占比':>8s}")
    for r in result["by_family"]:
        print(f"{r['family']:<16s} {r['files']:>9,} {r['pct_files']:>6.2f}% {r['bytes_human']:>10s} {r['pct_bytes']:>7.2f}%")
    print("\n== NZ 压缩类型 ==")
    for r in result["nz_types"]:
        print(f"{r['type']:<16s} {r['files']:>9,} {r['pct_files']:>6.2f}% {r['bytes_human']:>10s}")
    print("\n== 按 PAK（前 15，按文件数）==")
    print(f"{'PAK':<16s} {'文件数':>9s} {'体积':>10s}  主导格式族")
    for r in sorted(result["by_pak"], key=lambda x: -x["files"])[:15]:
        print(f"{r['pak']:<16s} {r['files']:>9,} {r['bytes_human']:>10s}  {r['top_family']}")
    cov = result["extraction_coverage"]
    print(f"\n== 提取覆盖 ==  manifests 记录 {cov['manifest_files']:,} 个文件已解包（{cov['pct']}%）")
    if args.loose:
        print("\n== 散目录 ==")
        for top, r in result["loose_dirs"].items():
            exts = ", ".join(f"{k}:{v}" for k, v in
                             sorted(r["ext"].items(), key=lambda x: -x[1])[:5])
            print(f"{top:<16s} {r['files']:>7,} 个文件 {human(r['bytes']):>10s}  [{exts}]")

    # ---- 写文件 ----
    if not args.no_write:
        os.makedirs(os.path.dirname(args.json), exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)

        lines = ["# 资源统计报告", "",
                 f"- PAK 数：**{o['paks']}**　文件总数：**{o['files']:,}**　存储体积：**{o['bytes_human']}**",
                 f"- 提取覆盖：manifests 记录 **{cov['manifest_files']:,}** 个文件已解包（{cov['pct']}%）",
                 "", "## 按格式族", "",
                 "| 格式族 | 文件数 | 占比 | 体积 | 体积占比 |", "|---|---:|---:|---:|---:|"]
        for r in result["by_family"]:
            lines.append(f"| {r['family']} | {r['files']:,} | {r['pct_files']}% | {r['bytes_human']} | {r['pct_bytes']}% |")
        lines += ["", "## 按扩展名", "", "| 扩展名 | 文件数 | 体积 |", "|---|---:|---:|"]
        for r in result["by_ext"]:
            lines.append(f"| {r['ext']} | {r['files']:,} | {r['bytes_human']} |")
        lines += ["", "## NZ 压缩类型", "", "| 类型 | 文件数 | 占比 | 体积 |", "|---|---:|---:|---:|"]
        for r in result["nz_types"]:
            lines.append(f"| {r['type']} | {r['files']:,} | {r['pct_files']}% | {r['bytes_human']} |")
        lines += ["", "## 按 PAK", "", "| PAK | 文件数 | 体积 | 主导格式族 | 格式族明细 |", "|---|---:|---:|---|---|"]
        for r in sorted(result["by_pak"], key=lambda x: -x["files"]):
            fams = ", ".join(f"{k}:{v:,}" for k, v in
                             sorted(r["families"].items(), key=lambda x: -x[1]))
            lines.append(f"| {r['pak']} | {r['files']:,} | {r['bytes_human']} | {r['top_family']} | {fams} |")
        if args.loose:
            lines += ["", "## 散目录（PAK 之外）", "", "| 目录 | 文件数 | 体积 | 主要扩展名 |", "|---|---:|---:|---|"]
            for top, r in result["loose_dirs"].items():
                exts = ", ".join(f"{k}:{v}" for k, v in
                                 sorted(r["ext"].items(), key=lambda x: -x[1])[:8])
                lines.append(f"| {top} | {r['files']:,} | {human(r['bytes'])} | {exts} |")
        lines += ["", "## 各 PAK 顶层目录分布", ""]
        for p in sorted(inv):
            tops = ", ".join(f"{k}:{v:,}" for k, v in result["top_dirs"][p])
            lines.append(f"- **{p}**：{tops}")
        with open(args.md, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print(f"\n已写出：{args.json}\n　　　{args.md}")


if __name__ == "__main__":
    sys.exit(main())
