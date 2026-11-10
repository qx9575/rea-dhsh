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


def _int_list(value: str) -> list[int]:
    out = []
    for part in (value or "").split(","):
        part = part.strip()
        if part:
            out.append(int(part))
    return out


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


def _star_row(cat, args, preset):
    """--star-general ID 或 --star-auto：解析星将形态行。"""
    if args.star_general:
        row = cat.star_general.get(args.star_general)
        if row is None:
            raise SystemExit(f"unknown star-general row: {args.star_general}")
        return row
    if args.star_auto:
        row = cat.star_general.default(preset.career, preset.gender)
        if row is None:
            print(f"[star-general] no default row for career={preset.career} "
                  f"gender={preset.gender}", file=sys.stderr)
        return row
    return None


def cmd_manifest(args) -> int:
    cat = _load(args)
    preset = cat.find_preset(args.career, args.advance, args.mount_tier, args.gender)
    if preset is None:
        print(f"preset not found: career={args.career} advance={args.advance} "
              f"mount={args.mount_tier} gender={args.gender}", file=sys.stderr)
        return 2
    star_row = _star_row(cat, args, preset)
    equip_overrides, equip_hues, equip_resolved = cat.equip_overrides(
        args.equip, star_general=star_row is not None)
    manual = _overrides(args.override)
    overrides = {**equip_overrides, **manual}
    hues = {field: 0 for field in manual}
    hues.update({k: v for k, v in equip_hues.items() if k not in manual})
    if args.effect_hue is not None:
        hues["effect"] = args.effect_hue
    if args.head_hue is not None:
        hues["head"] = args.head_hue
    manifest = cat.manifest(preset, args.action, args.direction,
                            body=args.body, head=args.head,
                            parse_frames=not args.no_frames,
                            overrides=overrides, hues=hues,
                            star_general=star_row,
                            mount_model=args.mount_model,
                            mount_id=args.mount_id)
    manifest["equip"] = equip_resolved
    out = Path(args.out) / (f"c{args.career}_a{args.advance}_m{args.mount_tier}"
                            f"_g{args.gender}_act{args.action}_dir{args.direction}.json")
    write_json(out, manifest)
    print(f"[manifest] {out}")
    if star_row is not None:
        print(f"[manifest] star-general {star_row.star_id}: "
              f"body={star_row.body} head={star_row.head} cloth={star_row.cloth} "
              f"weapon={star_row.weapon} shoe={star_row.shoe} cuff={star_row.cuff}")
    print(f"[manifest] parts={len(manifest['parts'])} missing={len(manifest['missing'])} "
          f"skills={len(manifest['skills'])}")
    if manifest.get("mount"):
        m = manifest["mount"]
        print(f"[manifest] mount model={m.get('mount_model')} "
              f"name={m.get('name')} horse={m.get('horse')} "
              f"harmor_head={m.get('harmor_head')} harmor_body={m.get('harmor_body')}")
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
    star_row = _star_row(cat, args, preset)
    equip_overrides, equip_hues, _equip_resolved = cat.equip_overrides(
        args.equip, star_general=star_row is not None)
    manual = _overrides(args.override)
    overrides = {**equip_overrides, **manual}
    hues = {field: 0 for field in manual}
    hues.update({k: v for k, v in equip_hues.items() if k not in manual})
    if args.effect_hue is not None:
        hues["effect"] = args.effect_hue
    if args.head_hue is not None:
        hues["head"] = args.head_hue
    actions = _int_list(args.actions) or [args.action]
    directions = _int_list(args.directions) or [args.direction]
    batch = []
    for action in actions:
        for direction in directions:
            assets = cat.parts(preset, action, direction,
                               body=args.body, head=args.head, parse_frames=True,
                               overrides=overrides, hues=hues,
                               star_general=star_row,
                               mount_model=args.mount_model,
                               mount_id=args.mount_id)
            frames, delay, diagnostics = doll_render.assemble(cat, assets)
            entry = {"action": action, "direction": direction,
                     "frames": len(frames), "delay": delay,
                     "skipped": diagnostics}
            if not frames:
                entry["status"] = "no_frames"
                batch.append(entry)
                print(f"[render] act{action}/dir{direction}: no frames",
                      file=sys.stderr)
                for item in diagnostics:
                    print(f"[render] skip {item}", file=sys.stderr)
                continue
            out_dir = Path(args.out) / (f"c{args.career}_a{args.advance}_m{args.mount_tier}"
                                        f"_g{args.gender}_act{action}_dir{direction}")
            paths = doll_render.save_frames(frames, delay, out_dir,
                                            make_gif=not args.no_gif)
            entry.update({"status": "ok", "wrote": len(paths),
                          "out": str(out_dir)})
            batch.append(entry)
            print(f"[render] act{action}/dir{direction}: {len(frames)} frames, "
                  f"{delay}ms -> {out_dir}")
            for item in diagnostics:
                print(f"[render] skip {item}")
    if len(batch) > 1:
        write_json(Path(args.out) / "render_batch.json", batch)
    if not any(entry["frames"] for entry in batch):
        return 3
    return 0


def cmd_validate(args) -> int:
    cat = _load(args, load_keys=False)
    by_field: dict[str, dict[str, int]] = {}
    by_career: dict[str, dict[str, int]] = {}
    samples = []
    total_parts = 0
    total_missing = 0
    total_mirror = 0
    total_npc = 0
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
                                                      "apg_missing": 0,
                                                      "mirror_resolved": 0,
                                                      "npc_resolved": 0})
            stats["parts"] += 1
            if asset.status in ("id_space_mismatch", "apg_missing"):
                stats[asset.status] += 1
            if asset.mirror_rule and asset.exists:
                total_mirror += 1
                stats["mirror_resolved"] += 1
            if asset.status == "found_npc":
                total_npc += 1
                stats["npc_resolved"] += 1
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
                        "resolved_model": asset.resolved_model,
                        "mirror_rule": asset.mirror_rule,
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
        "mirror_resolved": total_mirror,
        "npc_resolved": total_npc,
        "by_field": by_field,
        "by_career": by_career,
        "missing_samples": samples,
        "effects": cat.effect_coverage(),
    }
    out = Path(args.out) / "validate.json"
    write_json(out, result)
    print(f"[validate] {len(presets)} presets, {total_parts} parts, "
          f"missing={total_missing}, resmirror_resolved={total_mirror}, "
          f"npc_resolved={total_npc} -> {out}")
    for field, stats in sorted(by_field.items()):
        if stats["missing"] or stats.get("mirror_resolved") or stats.get("npc_resolved"):
            print(f"  {field}: {stats['missing']}/{stats['parts']} "
                  f"(id_space_mismatch={stats.get('id_space_mismatch', 0)}, "
                  f"apg_missing={stats.get('apg_missing', 0)}, "
                  f"mirror_resolved={stats.get('mirror_resolved', 0)}, "
                  f"npc_resolved={stats.get('npc_resolved', 0)})")
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
    parser.add_argument("--mount-model", type=int, default=None,
                        help="直接指定坐骑资源模型（如赤炎龙 6100、冰龙 14650）")
    parser.add_argument("--mount-id", type=int, default=None,
                        help="horseinfo 坐骑 ID，用于读取名称/职业/等级元数据")
    parser.add_argument("--gender", type=int, default=0)
    parser.add_argument("--action", type=int, default=10)
    parser.add_argument("--direction", type=int, default=0)
    parser.add_argument("--actions", default="",
                        help="render: 逗号分隔动作（如 10,20,30）；为空用 --action")
    parser.add_argument("--directions", default="",
                        help="render: 逗号分隔朝向（如 0,1,2）；为空用 --direction")
    parser.add_argument("--body", type=int, default=None,
                        help="身体模型；默认 career*100 + 50*gender（如术士女=450）")
    parser.add_argument("--head", type=int, default=105)
    parser.add_argument("--star-general", type=int, default=None,
                        help="星将/星神形态行 ID（bodyidequal 36xxx/38xxx，如浪子女 36615）")
    parser.add_argument("--star-auto", action="store_true",
                        help="按职业/性别自动选择星将形态行（目前已知浪子 36600/36615）")
    parser.add_argument("--no-frames", action="store_true")
    parser.add_argument("--no-gif", action="store_true")
    parser.add_argument("--override", action="append", metavar="PART=MODEL",
                        help="覆盖部件模型，如 mask=7 / gloves=50 / weapon=100；可重复")
    parser.add_argument("--equip", action="append", type=int, metavar="ITEM_ID",
                        help="按 itemSpecData 物品 ID 覆盖装备部件；可重复")
    parser.add_argument("--effect-hue", type=int, default=None,
                        help="武器光效（effect 层）色相角度；默认用 ITEM_EFFECT")
    parser.add_argument("--head-hue", type=int, default=None,
                        help="头部/发型色相角度（用于星将形态自定义发色）")
    parser.add_argument("--limit", type=int, default=0,
                        help="validate 只检查前 N 条预设（0 = 全部）")
    args = parser.parse_args(argv)
    return {"manifest": cmd_manifest, "presets": cmd_presets,
            "render": cmd_render, "validate": cmd_validate}[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())
