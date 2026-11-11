import tempfile
import unittest
import zipfile
from pathlib import Path

from package_client import build_package


class PackageClientTest(unittest.TestCase):
    def make_inputs(self, root):
        client = root / "client.exe"
        readme = root / "README.md"
        launcher = root / "run_client.cmd"
        cache = root / "map-cache"
        client.write_bytes(b"native client fixture")
        readme.write_text("readme", encoding="utf-8")
        launcher.write_text("@echo off\r\n", encoding="utf-8")
        (cache / "map/YingTianFu").mkdir(parents=True)
        (cache / "client_map_cache.json").write_text("{}", encoding="utf-8")
        (cache / "map/YingTianFu/background.png").write_bytes(b"png")
        return client, readme, launcher, cache

    def test_package_contains_client_launcher_docs_cache_and_zip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "dist/dhsh-client"
            archive = root / "dist/dhsh-client.zip"
            build_package(*self.make_inputs(root), output, archive)
            self.assertEqual((output / "dhsh_client.exe").read_bytes(),
                b"native client fixture")
            self.assertEqual((output / "README.md").read_text(encoding="utf-8"),
                "readme")
            self.assertTrue((output / "run_client.cmd").is_file())
            self.assertTrue((output / "map-cache/client_map_cache.json").is_file())
            self.assertTrue((output / "map-cache/map/YingTianFu/background.png").is_file())
            with zipfile.ZipFile(archive) as packaged:
                self.assertEqual(set(packaged.namelist()), {
                    "dhsh-client/dhsh_client.exe",
                    "dhsh-client/README.md",
                    "dhsh-client/run_client.cmd",
                    "dhsh-client/map-cache/client_map_cache.json",
                    "dhsh-client/map-cache/map/YingTianFu/background.png",
                })

    def test_existing_output_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "dist/dhsh-client"
            archive = root / "dist/dhsh-client.zip"
            inputs = self.make_inputs(root)
            build_package(*inputs, output, archive)
            sentinel = output / "dhsh_client.exe"
            sentinel.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                build_package(*inputs, output, archive)
            self.assertEqual(sentinel.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
