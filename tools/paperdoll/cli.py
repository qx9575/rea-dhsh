"""纸娃娃清单 CLI。

用法：
  python tools/paperdoll/cli.py manifest --career 1 --advance 0 --mount-tier 1 --gender 0
  python tools/paperdoll/cli.py presets
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from art_pipeline.common import DEFAULT_SHDATA, DEFAULT_OUT, write_json  # noqa: E402
from paperdoll.catalog import PaperDollCatalog  # noqa: E402
from paperdoll import render as doll_render  # noqa: E402


def _load(args, load_keys: bool = True) -> PaperDollCatalog:
    inv = Path(args.inventory) if args.inventory else None
    return PaperDollCatalog.load(Path(args.shdata), inv, load_keys=load_keys)


def _overrides(pairs) -> dict[str, int]:
    aliases = {
        "shoe": "shoes", "shoes": "shoes",
        "gloves": "gloves",
        "lcuff": "gloves_l", "rcuff": "gloves_r",
        "harmorhead": "harmor_head", "harmorbody": "harmor_body",
    }
    out: dict[str, int] = {}
    for item in pairs or []:
        if "=" not in item:
            raise SystemExit(f"bad override (expected part=model): {item}")
        part, value = item.split("=", 1)
        part = aliases.get(part.strip().lower(), part.strip())
        try:
            out[part] = int(value)
        except ValueError:
            raise SystemExit(f"bad override model: {item}") from None
    return out


def cmd_manifest(args) -> int:
    cat = _load(args)
    preset = cat.find_preset(args.career, args.advance, args.mount_tier, args.gender)
    if preset is None:
        print(f"preset not found: career={args.career} advance={args.advance} "
              f"mount={args.mount_tier} gender={args.gender}", file=sys.stderr)
        return 2
    equip_overrides, equip_resolved = cat.equip_overrides(args.equip)
    overrides = {**equip_overrides, **_overrides(args.override)}
    manifest = cat.manifest(preset, args.action, args.direction,
                            body=args.body, head=args.head,
                            parse_frames=not args.no_frames,
                            overrides=overrides)
    manifest["equip"] = equip_resolved
    out = Path(args.out) / (f"c{args.career}_a{args.advance}_m{args.mount_tier}"
                            f"_g{args.gender}_act{args.action}_dir{args.direction}.json")
    write_json(out, manifest)
    print(f"[manifest] {out}")
    print(f"[manifest] parts={len(manifest['parts'])} missing={len(manifest['missing'])} "
          f"skills={len(manifest['skills'])}")
    for item in equip_resolved:
        print(f"[manifest] equip {item.get('item_id')} {item.get('name', '')}: "
              f"{item['status']}")
    return 0


def cmd_render(args) -> int:
    cat = _load(args)
    preset = cat.find_preset(args.career, args.advance, args.mount_tier, args.gender)
    if preset is None:
        print(f"preset not found: career={args.career} advance={args.advance} "
              f"mount={args.mount_tier} gender={args.gender}", file=sys.stderr)
        return 2
    equip_overrides, _equip_resolved = cat.equip_overrides(args.equip)
    overrides = {**equip_overrides, **_overrides(args.override)}
    assets = cat.parts(preset, args.action, args.direction,
                       body=args.body, head=args.head, parse_frames=True,
                       overrides=overrides)
    frames, delay, diagnostics = doll_render.assemble(cat, assets)
    if not frames:
        print("no frames assembled", file=sys.stderr)
        for item in diagnostics:
            print("  " + item, file=sys.stderr)
        return 3
    out_dir = Path(args.out) / (f"c{args.career}_a{args.advance}_m{args.mount_tier}"
                                f"_g{args.gender}_act{args.action}_dir{args.direction}")
    paths = doll_render.save_frames(frames, delay, out_dir, make_gif=not args.no_gif)
    print(f"[render] {len(frames)} frames, {delay}ms -> {out_dir}")
    print(f"[render] wrote {len(paths)} files")
    for item in diagnostics:
        print(f"[render] skip {item}")
    return 0


def cmd_validate(args) -> int:
    cat = _load(args, load_keys=False)
    by_field: dict[str, dict[str, int]] = {}
    by_career: dict[str, dict[str, int]] = {}
    samples = []
    total_parts = 0
    total_missing = 0
    presets = cat.presets[:args.limit] if args.limit else cat.presets
    for preset in presets:
        assets = cat.parts(preset, args.action, args.direction,
                           body=args.body, head=args.head, parse_frames=False)
        career_key = str(preset.career)
        by_career.setdefault(career_key, {"parts": 0, "missing": 0})
        for asset in assets:
            total_parts += 1
            by_career[career_key]["parts"] += 1
            stats = by_field.setdefault(asset.field, {"parts": 0, "missing": 0,
                                                      "id_space_mismatch": 0,
                                                      "apg_missing": 0})
            stats["parts"] += 1
            if asset.status in ("id_space_mismatch", "apg_missing"):
                stats[asset.status] += 1
            if not asset.exists:
                total_missing += 1
                by_career[career_key]["missing"] += 1
                stats["missing"] += 1
                if len(samples) < 500:
                    samples.append({
                        "career": preset.career,
                        "advance": preset.advance,
                        "mount_tier": preset.mount_tier,
                        "gender": preset.gender,
                        "field": asset.field,
                        "part": asset.part,
                        "model": asset.model,
                        "status": asset.status,
                        "found_in": asset.found_in,
                        "pak": asset.pak,
                        "apg": asset.apg,
                        "note": asset.note,
                    })
    result = {
        "presets": len(presets),
        "parts_checked": total_parts,
        "missing": total_missing,
        "by_field": by_field,
        "by_career": by_career,
        "missing_samples": samples,
        "effects": cat.effect_coverage(),
    }
    out = Path(args.out) / "validate.json"
    write_json(out, result)
    print(f"[validate] {len(presets)} presets, {total_parts} parts, "
          f"missing={total_missing} -> {out}")
    for field, stats in sorted(by_field.items()):
        if stats["missing"]:
            print(f"  {field}: {stats['missing']}/{stats['parts']} "
                  f"(id_space_mismatch={stats.get('id_space_mismatch', 0)}, "
                  f"apg_missing={stats.get('apg_missing', 0)})")
    effects = result["effects"]
    print(f"[validate] skill effects: {effects['resolved']}/{effects['effect_ids']} "
          f"resolved; missing={effects['missing']}")
    return 0


def cmd_presets(args) -> int:
    cat = _load(args)
    rows = []
    for p in cat.presets:
        rows.append({
            "career": p.career,
            "advance": p.advance,
            "mount_tier": p.mount_tier,
            "gender": p.gender,
            "cloth": p.cloth,
            "gloves": p.gloves,
            "shoes": p.shoes,
            "weapon": p.weapon,
            "horse": p.horse,
            "harmor_head": p.harmor_head,
            "harmor_body": p.harmor_body,
            "hero_branch": p.hero_branch,
            "stargod_model": p.stargod_model,
        })
    out = Path(args.out) / "presets.json"
    write_json(out, rows)
    print(f"[presets] {len(rows)} -> {out}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="纸娃娃职业预设/资源清单")
    parser.add_argument("command", choices=["manifest", "presets", "render", "validate"])
    parser.add_argument("--shdata", default=str(DEFAULT_SHDATA))
    parser.add_argument("--out", default=str(Path(DEFAULT_OUT) / "paperdoll"))
    parser.add_argument("--inventory", default="")
    parser.add_argument("--career", type=int, default=1)
    parser.add_argument("--advance", type=int, default=0)
    parser.add_argument("--mount-tier", type=int, default=0)
    parser.add_argument("--gender", type=int, default=0)
    parser.add_argument("--action", type=int, default=10)
    parser.add_argument("--direction", type=int, default=0)
    parser.add_argument("--body", type=int, default=200)
    parser.add_argument("--head", type=int, default=105)
    parser.add_argument("--no-frames", action="store_true")
    parser.add_argument("--no-gif", action="store_true")
    parser.add_argument("--override", action="append", metavar="PART=MODEL",
                        help="覆盖部件模型，如 mask=7 / gloves=50 / weapon=100；可重复")
    parser.add_argument("--equip", action="append", type=int, metavar="ITEM_ID",
                        help="按 itemSpecData 物品 ID 覆盖装备部件；可重复")
    parser.add_argument("--limit", type=int, default=0,
                        help="validate 只检查前 N 条预设（0 = 全部）")
    args = parser.parse_args(argv)
    return {"manifest": cmd_manifest, "presets": cmd_presets,
            "render": cmd_render, "validate": cmd_validate}[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())
