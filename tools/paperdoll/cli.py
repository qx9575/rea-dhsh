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


def _load(args) -> PaperDollCatalog:
    inv = Path(args.inventory) if args.inventory else None
    return PaperDollCatalog.load(Path(args.shdata), inv)


def cmd_manifest(args) -> int:
    cat = _load(args)
    preset = cat.find_preset(args.career, args.advance, args.mount_tier, args.gender)
    if preset is None:
        print(f"preset not found: career={args.career} advance={args.advance} "
              f"mount={args.mount_tier} gender={args.gender}", file=sys.stderr)
        return 2
    manifest = cat.manifest(preset, args.action, args.direction,
                            body=args.body, head=args.head,
                            parse_frames=not args.no_frames)
    out = Path(args.out) / (f"c{args.career}_a{args.advance}_m{args.mount_tier}"
                            f"_g{args.gender}_act{args.action}_dir{args.direction}.json")
    write_json(out, manifest)
    print(f"[manifest] {out}")
    print(f"[manifest] parts={len(manifest['parts'])} missing={len(manifest['missing'])} "
          f"skills={len(manifest['skills'])}")
    return 0


def cmd_render(args) -> int:
    cat = _load(args)
    preset = cat.find_preset(args.career, args.advance, args.mount_tier, args.gender)
    if preset is None:
        print(f"preset not found: career={args.career} advance={args.advance} "
              f"mount={args.mount_tier} gender={args.gender}", file=sys.stderr)
        return 2
    assets = cat.parts(preset, args.action, args.direction,
                       body=args.body, head=args.head, parse_frames=True)
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
    parser.add_argument("command", choices=["manifest", "presets", "render"])
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
    args = parser.parse_args(argv)
    return {"manifest": cmd_manifest, "presets": cmd_presets,
            "render": cmd_render}[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())
