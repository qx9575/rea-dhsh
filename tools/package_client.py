import argparse
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def build_package(client, readme, launcher, cache, output_dir, zip_path):
    client = Path(client).resolve()
    readme = Path(readme).resolve()
    launcher = Path(launcher).resolve()
    cache = Path(cache).resolve()
    output_dir = Path(output_dir).resolve()
    zip_path = Path(zip_path).resolve() if zip_path is not None else None
    for path in (client, readme, launcher):
        if not path.is_file():
            raise FileNotFoundError(path)
    if not cache.is_dir():
        raise FileNotFoundError(cache)
    if output_dir.exists():
        raise FileExistsError(f"output directory already exists: {output_dir}")
    if zip_path is not None:
        if zip_path.exists():
            raise FileExistsError(f"ZIP already exists: {zip_path}")
        if zip_path.is_relative_to(output_dir):
            raise ValueError("ZIP must be outside the package directory")

    output_created = False
    zip_created = False
    try:
        output_dir.mkdir(parents=True)
        output_created = True
        shutil.copy2(client, output_dir / "dhsh_client.exe")
        shutil.copy2(readme, output_dir / "README.md")
        shutil.copy2(launcher, output_dir / "run_client.cmd")
        shutil.copytree(cache, output_dir / "map-cache")
        if zip_path is not None:
            zip_path.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(zip_path, "x", zipfile.ZIP_DEFLATED,
                    compresslevel=6) as archive:
                zip_created = True
                for path in sorted(output_dir.rglob("*")):
                    if path.is_file():
                        archive.write(path, path.relative_to(output_dir.parent))
        return output_dir, zip_path
    except Exception:
        if zip_created:
            zip_path.unlink(missing_ok=True)
        if output_created:
            shutil.rmtree(output_dir)
        raise


def parse_args():
    parser = argparse.ArgumentParser(
        description="Package the native client, launcher, README and map cache.")
    parser.add_argument("--client", type=Path,
        default=ROOT / "client/build/Release/dhsh_client.exe")
    parser.add_argument("--cache", type=Path,
        default=ROOT / "client/build/map-cache")
    parser.add_argument("--output-dir", type=Path,
        default=ROOT / "client/dist/dhsh-client")
    parser.add_argument("--zip", dest="zip_path", type=Path,
        help="ZIP output path; defaults to <output-dir>.zip")
    parser.add_argument("--no-zip", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    zip_path = None if args.no_zip else (
        args.zip_path or args.output_dir.with_suffix(".zip"))
    output_dir, archive = build_package(
        args.client, ROOT / "client/README.md", ROOT / "client/run_client.cmd",
        args.cache, args.output_dir, zip_path)
    print(f"package={output_dir}")
    if archive is not None:
        print(f"zip={archive}")


if __name__ == "__main__":
    main()
