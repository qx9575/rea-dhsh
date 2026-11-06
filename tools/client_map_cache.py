#!/usr/bin/env python
"""Export selected map backgrounds for the native client cache.

The client decodes standard images with WIC, but map backgrounds are the
private ``FF D8 FF C8`` JPEG variant.  This tool uses the main analysis
project's decoder (``tools/dhsh/formats/special_jpg.py``) to decode only the
explicitly selected maps, writes PNG files under their logical PAK path with
a ``.png`` suffix, and records source path, size, PAK mtime and SHA-256 in a
manifest next to the cache.

A cache hit re-hashes the existing PNG; if its bytes changed the background is
re-exported, and the PAK size/SHA/mtime and MAP metadata are refreshed from the
current archives instead of being copied from the old manifest.

Only selected maps are exported; there is no "all maps" mode.  The default
selection is the M1 map, YingTianFu.  The source tree is opened read-only.

Example (from the native-client worktree):

    python tools/client_map_cache.py \
        --shdata D:/software/dhsh/shdata \
        --map YingTianFu \
        --cache client/cache/map

Output for the M1 background:

    <cache>/map/YingTianFu/YingTianFu_10Year.png
    <cache>/client_map_cache.json
"""
from __future__ import annotations

import argparse
import datetime as _datetime
import hashlib
import json
import os
import sys
from pathlib import Path

DEFAULT_REPO_ROOT = Path(
    os.environ.get("DHSH_REPO_ROOT", r"D:\ai-work-flow\rea-dhsh"))
DEFAULT_SHDATA = Path(
    os.environ.get("DHSH_SHDATA", r"D:\software\dhsh\shdata"))
DEFAULT_MAP = "YingTianFu"
MANIFEST_NAME = "client_map_cache.json"
FORMAT_VERSION = 1


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def iso_utc(ns: int) -> str:
    stamp = _datetime.datetime.fromtimestamp(
        ns / 1_000_000_000, tz=_datetime.timezone.utc)
    return stamp.isoformat(timespec="seconds")


def slash_path(path: str) -> str:
    return path.replace("\\", "/").lstrip("/")


def lookup_key(path: str) -> str:
    return slash_path(path).lower()


def path_variants(path: str) -> list[str]:
    """Literal path, map1 sibling, then the stripped form."""
    rel = slash_path(path)
    if not rel:
        return []
    variants = [rel]
    lower = rel.lower()
    if lower.startswith("map/"):
        rest = rel[4:]
        variants.extend(["map1/" + rest, rest])
    elif lower.startswith("map1/"):
        rest = rel[5:]
        variants.extend(["map/" + rest, rest])
    deduped = []
    seen = set()
    for candidate in variants:
        key = candidate.lower()
        if key not in seen:
            seen.add(key)
            deduped.append(candidate)
    return deduped


def safe_output_relative(logical_path: str) -> str:
    """Turn a MAP BACKGROUND path into a safe relative cache path.

    Rejects POSIX absolute paths, Windows drive/UNC paths, NUL bytes, drive
    or stream colons, and any ``..`` component.  The result is always a
    relative path with ``/`` separators and a ``.png`` suffix.
    """
    normalized = logical_path.replace("\\", "/")
    if not normalized:
        raise ValueError("empty background path")
    if normalized.startswith("/"):
        raise ValueError(
            f"absolute or UNC background path: {logical_path!r}")
    if (len(normalized) >= 2 and normalized[1] == ":"
            and normalized[0].isascii() and normalized[0].isalpha()):
        raise ValueError(
            f"Windows drive background path: {logical_path!r}")
    if "\x00" in normalized:
        raise ValueError(f"NUL in background path: {logical_path!r}")
    parts = [part for part in normalized.split("/") if part not in ("", ".")]
    if not parts or any(part == ".." for part in parts):
        raise ValueError(f"unsafe background path: {logical_path!r}")
    if any(":" in part for part in parts):
        raise ValueError(
            f"Windows drive or stream background path: {logical_path!r}")
    return os.path.splitext("/".join(parts))[0] + ".png"


def cache_output_path(cache_root: Path, logical_path: str) -> tuple[str, Path]:
    """Resolve a cache output path and prove it stays under ``cache_root``.

    ``safe_output_relative`` rejects absolute/drive/UNC/traversal input.  The
    joined path is resolved again so a symlink or junction inside the cache
    root cannot redirect the write outside it.
    """
    relative = safe_output_relative(logical_path)
    relative_path = Path(relative)
    if relative_path.is_absolute() or relative_path.drive:
        raise ValueError(f"unsafe background path: {logical_path!r}")
    root = cache_root.resolve()
    output = (root / relative_path).resolve()
    if not output.is_relative_to(root):
        raise ValueError(
            f"background output escapes the cache root: {logical_path!r}")
    return relative, output


def load_main_tools(repo_root: Path):
    """Import PAK/NZ/map/JPEG helpers from the main analysis checkout."""
    tools_root = (repo_root / "tools").resolve()
    special_path = tools_root / "dhsh" / "formats" / "special_jpg.py"
    if not special_path.is_file():
        raise SystemExit(
            f"main project decoder not found: {special_path}\n"
            "pass --repo-root or set DHSH_REPO_ROOT")

    # The script normally lives under a worktree whose tools/ directory is
    # sys.path[0]; force the main checkout's package to win.
    for name in list(sys.modules):
        if name == "dhsh" or name.startswith("dhsh."):
            del sys.modules[name]
    sys.path.insert(0, str(tools_root))

    from dhsh import nz as dhnz
    from dhsh.formats import special_jpg
    from dhsh.formats.map import parse_map_ini
    from dhsh.pak import iter_pak

    module_path = Path(special_jpg.__file__).resolve()
    if not module_path.is_relative_to(tools_root):
        raise SystemExit(
            f"imported special_jpg from {module_path}, expected {tools_root}")
    return dhnz, iter_pak, parse_map_ini, special_jpg


def index_pak(pak_path: Path, iter_pak) -> dict[str, object]:
    index: dict[str, object] = {}
    for entry in iter_pak(pak_path):
        index.setdefault(lookup_key(entry.path), entry)
    return index


def read_entry(pak_path: Path, entry) -> bytes:
    with open(pak_path, "rb") as source:
        source.seek(entry.offset)
        stored = source.read(entry.size)
    if len(stored) != entry.size:
        raise ValueError(
            f"truncated PAK entry {entry.path!r}: "
            f"expected {entry.size} bytes, read {len(stored)}")
    return stored


def decode_nz(dhnz, stored: bytes, label: str) -> tuple[bytes, object]:
    header = dhnz.parse_header(stored)
    if header is None:
        raise ValueError(f"{label} is not NZ-wrapped")
    begin = dhnz.HEADER_SIZE
    end = begin + header.size2
    if end > len(stored):
        raise ValueError(f"{label} has a truncated NZ payload")
    payload = stored[begin:end]
    decoded = dhnz.decompress(header.kind, payload, header.size1)
    if len(decoded) != header.size1:
        raise ValueError(
            f"{label} NZ size mismatch: {len(decoded)} != {header.size1}")
    return decoded, header


def find_entry(index: dict[str, object], logical_path: str):
    for candidate in path_variants(logical_path):
        hit = index.get(candidate.lower())
        if hit is not None:
            return hit
    return None


def load_manifest(path: Path) -> dict:
    try:
        with open(path, encoding="utf-8") as source:
            data = json.load(source)
    except FileNotFoundError:
        return {"format_version": FORMAT_VERSION, "maps": {}}
    except (OSError, ValueError) as error:
        print(f"warning: ignoring unreadable manifest {path}: {error}",
              file=sys.stderr)
        return {"format_version": FORMAT_VERSION, "maps": {}}
    if not isinstance(data, dict):
        return {"format_version": FORMAT_VERSION, "maps": {}}
    maps = data.get("maps")
    if not isinstance(maps, dict):
        data["maps"] = {}
    return data


def write_manifest(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    try:
        with open(temporary, "w", encoding="utf-8", newline="\n") as output:
            json.dump(data, output, ensure_ascii=False, indent=2)
            output.write("\n")
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def build_source_record(pak_path: Path, entry, stored: bytes, decoded: bytes,
                        header) -> dict:
    stat = pak_path.stat()
    return {
        "pak": pak_path.name,
        "pak_path": str(pak_path.resolve()),
        "entry_path": slash_path(entry.path),
        "stored_size": len(stored),
        "stored_sha256": sha256_bytes(stored),
        "decoded_size": len(decoded),
        "decoded_sha256": sha256_bytes(decoded),
        "nz_kind": f"0x{header.kind:04x}",
        "pak_size": stat.st_size,
        "pak_mtime_ns": stat.st_mtime_ns,
        "pak_mtime": iso_utc(stat.st_mtime_ns),
    }


def build_map_record(
    name: str,
    output_rel: str,
    jpg,
    output_size: int,
    output_sha: str,
    map_pak: Path,
    background_entry,
    stored: bytes,
    payload: bytes,
    header,
    config_pak: Path,
    ini_entry,
    ini_stored: bytes,
    ini_payload: bytes,
    ini_header,
    ini_key: str,
    background: str,
) -> dict:
    """Build one manifest record from the current PAK bytes and output file.

    Both the cache-hit and the re-export path use this helper so a hit never
    returns a stale PAK size, SHA, mtime or MAP metadata record.
    """
    background_record = build_source_record(
        map_pak, background_entry, stored, payload, header)
    background_record.update({
        "logical_path": slash_path(background),
        "source": f"{map_pak.name}:{slash_path(background_entry.path)}",
        "width": jpg.width,
        "height": jpg.height,
        "output": output_rel,
        "output_size": output_size,
        "output_sha256": output_sha,
    })
    ini_record = build_source_record(
        config_pak, ini_entry, ini_stored, ini_payload, ini_header)
    ini_record["logical_path"] = slash_path(ini_key)
    return {
        "map": name,
        "map_ini": ini_record,
        "background": background_record,
    }


def export_map(name: str, args, dhnz, parse_map_ini,
               special_jpg, config_index, map_pak: Path,
               map_index, cache_root: Path, old_entry):
    map_candidates = [
        f"map/{name}.map",
        f"config/map/{name}.map",
        f"{name}.map",
    ]
    ini_entry = None
    ini_key = None
    for candidate in map_candidates:
        ini_entry = config_index.get(candidate.lower())
        if ini_entry is not None:
            ini_key = candidate
            break
    if ini_entry is None:
        raise ValueError(
            f"MAP entry not found for {name!r}; tried {', '.join(map_candidates)}")

    config_pak = args.shdata / "config.pak"
    ini_stored = read_entry(config_pak, ini_entry)
    ini_payload, ini_header = decode_nz(
        dhnz, ini_stored, f"config.pak:{ini_entry.path}")
    ini = parse_map_ini(ini_payload)
    background = ini.get("base", {}).get("BACKGROUND", "").strip()
    if not background:
        raise ValueError(f"MAP {name!r} has no BACKGROUND")
    output_rel, output_path = cache_output_path(cache_root, background)

    background_entry = find_entry(map_index, background)
    if background_entry is None:
        raise ValueError(f"background entry not found: {background}")

    stored = read_entry(map_pak, background_entry)
    payload, header = decode_nz(
        dhnz, stored, f"{map_pak.name}:{background_entry.path}")
    if not payload.startswith(special_jpg.MAGIC):
        raise ValueError(
            f"background {background_entry.path!r} is not the private "
            f"FF D8 FF C8 JPEG format")

    decoded_sha = sha256_bytes(payload)

    jpg = special_jpg.parse(payload)
    if jpg.width <= 0 or jpg.height <= 0:
        raise ValueError(
            f"background {background_entry.path!r} has invalid dimensions")

    old_background = {}
    if isinstance(old_entry, dict):
        candidate = old_entry.get("background")
        if isinstance(candidate, dict):
            old_background = candidate

    # A cache hit requires the current source SHA as well as the current
    # output bytes.  The output hash is recomputed (never trusted from the
    # manifest) and a mismatch forces a re-export.
    if (not args.force
            and old_background.get("decoded_sha256") == decoded_sha
            and old_background.get("output") == output_rel
            and output_path.is_file()):
        output_size = output_path.stat().st_size
        output_sha = sha256_bytes(output_path.read_bytes())
        if (old_background.get("output_size") == output_size
                and old_background.get("output_sha256") == output_sha):
            result = build_map_record(
                name, output_rel, jpg, output_size, output_sha,
                map_pak, background_entry, stored, payload, header,
                config_pak, ini_entry, ini_stored, ini_payload, ini_header,
                ini_key, background)
            print(f"{name}: cache up to date ({output_rel})")
            return result

    if args.dry_run:
        print(f"{name}: would decode {background_entry.path} -> {output_rel}")
        return old_entry

    from PIL import Image

    jpg_image = special_jpg.decode(jpg, fast=not args.no_fast)
    if jpg_image.shape[:2] != (jpg.height, jpg.width):
        raise ValueError(
            f"decoder shape {jpg_image.shape} does not match "
            f"{jpg.width}x{jpg.height}")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(
        output_path.name + f".tmp.{os.getpid()}")
    try:
        Image.fromarray(jpg_image).save(
            temporary, format="PNG", compress_level=args.png_level)
        os.replace(temporary, output_path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass

    output_size = output_path.stat().st_size
    output_sha = sha256_bytes(output_path.read_bytes())
    result = build_map_record(
        name, output_rel, jpg, output_size, output_sha,
        map_pak, background_entry, stored, payload, header,
        config_pak, ini_entry, ini_stored, ini_payload, ini_header,
        ini_key, background)
    print(f"{name}: {jpg.width}x{jpg.height} -> {output_rel} "
          f"({output_size / 1048576:.1f} MiB)")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Export selected map backgrounds for the native client "
                    "cache; never exports the full corpus.")
    parser.add_argument("--repo-root", type=Path, default=DEFAULT_REPO_ROOT,
                        help="main analysis checkout containing tools/dhsh "
                             f"(default: {DEFAULT_REPO_ROOT})")
    parser.add_argument("--shdata", type=Path, default=DEFAULT_SHDATA,
                        help=f"read-only shdata directory (default: {DEFAULT_SHDATA})")
    parser.add_argument("--map", action="append", dest="maps", default=None,
                        help=f"map stem to export (repeatable; default: {DEFAULT_MAP})")
    parser.add_argument("--cache", type=Path, required=True,
                        help="cache root to write background PNG/manifest into")
    parser.add_argument("--manifest", type=Path, default=None,
                        help=f"manifest path (default: <cache>/{MANIFEST_NAME})")
    parser.add_argument("--png-level", type=int, default=6,
                        help="PNG zlib compression level 0-9 (default: 6)")
    parser.add_argument("--no-fast", action="store_true",
                        help="disable the optional numba JPEG MCU kernel")
    parser.add_argument("--force", action="store_true",
                        help="re-export even when the source SHA is unchanged")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    args.repo_root = args.repo_root.resolve()
    args.shdata = args.shdata.resolve()
    args.cache = args.cache.resolve()
    manifest_path = (args.manifest or (args.cache / MANIFEST_NAME)).resolve()
    maps = args.maps or [DEFAULT_MAP]
    unique_maps = []
    seen = set()
    for name in maps:
        if name.lower() not in seen:
            seen.add(name.lower())
            unique_maps.append(name)
    args.maps = unique_maps

    if not 0 <= args.png_level <= 9:
        parser.error("--png-level must be between 0 and 9")
    if args.cache.is_relative_to(args.shdata):
        parser.error("--cache must not be inside the read-only shdata tree")
    if manifest_path.is_relative_to(args.shdata):
        parser.error("--manifest must not be inside the read-only shdata tree")
    for pak in ("config.pak", "map.pak"):
        if not (args.shdata / pak).is_file():
            parser.error(f"missing {pak} under {args.shdata}")

    dhnz, iter_pak, parse_map_ini, special_jpg = load_main_tools(
        args.repo_root)

    config_pak = args.shdata / "config.pak"
    map_pak = args.shdata / "map.pak"
    print(f"indexing {config_pak.name} and {map_pak.name} ...")
    config_index = index_pak(config_pak, iter_pak)
    map_index = index_pak(map_pak, iter_pak)

    manifest = load_manifest(manifest_path)
    if manifest.get("format_version") != FORMAT_VERSION:
        manifest = {"format_version": FORMAT_VERSION, "maps": {}}
    manifest["generated_at"] = _datetime.datetime.now(
        tz=_datetime.timezone.utc).isoformat(timespec="seconds")
    manifest["repo_root"] = str(args.repo_root)
    manifest["shdata"] = str(args.shdata)
    result_maps = manifest.setdefault("maps", {})

    errors = 0
    for name in args.maps:
        old_entry = result_maps.get(name)
        try:
            entry = export_map(
                name, args, dhnz, parse_map_ini, special_jpg,
                config_index, map_pak, map_index, args.cache, old_entry)
            if entry is not None:
                result_maps[name] = entry
        except Exception as error:  # noqa: BLE001
            errors += 1
            print(f"FAIL: {name}: {type(error).__name__}: {error}",
                  file=sys.stderr)

    if not args.dry_run:
        write_manifest(manifest_path, manifest)
        print(f"manifest: {manifest_path}")
    print(f"done: {len(args.maps) - errors}/{len(args.maps)} map(s), "
          f"{errors} error(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
