#!/usr/bin/env python3
"""The ciderdarling-bin recipe generated from a release manifest."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("aur_package", ROOT / "tools/cider/aur-package.py")
aur = importlib.util.module_from_spec(spec)
spec.loader.exec_module(aur)

REVISION = "a8dae5f8e6daf6ec0245f94a9ba97c0d0cc4aeb7"
MANIFEST = {
    "schema_version": 1, "name": "CiderDarling", "architecture": "x86_64", "flavor": "arch",
    "source_revision": REVISION, "source_date": 1791820000,
    "archive": f"ciderdarling-{REVISION}-x86_64-arch.tar.xz", "archive_sha256": "ab" * 32,
    "shared_libraries": ["libEGL.so.1", "libGL.so.1", "libc.so.6", "libgcc_s.so.1", "libm.so.6", "libstdc++.so.6"],
    "native_libraries": ["libavcodec.so.63", "libfuse.so.2", "libjpeg.so.8", "libpng16.so.16", "libX11.so.6"],
}


class AurPackageTests(unittest.TestCase):
    def render(self, manifest, *extra):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "manifest.json").write_text(json.dumps(manifest))
            result = subprocess.run(["python3", ROOT / "tools/cider/aur-package.py", "--manifest",
                                     directory / "manifest.json", "--output", directory / "out", *extra],
                                    capture_output=True, text=True)
            files = {path.name: path.read_text() for path in (directory / "out").glob("*")} \
                if (directory / "out").is_dir() else {}
            return result, files

    def test_recipe_installs_the_release_archive_setuid(self):
        result, files = self.render(MANIFEST)
        self.assertEqual(result.returncode, 0, result.stderr)
        recipe = files["PKGBUILD"]
        self.assertIn("pkgver=20261012.a8dae5f8e\n", recipe)
        self.assertIn(f"_revision={REVISION}\n", recipe)
        self.assertIn(f"/releases/download/bin-{REVISION[:12]}/{MANIFEST['archive']}", recipe)
        self.assertIn(f"sha256sums=('{'ab' * 32}')", recipe)
        self.assertIn('chmod 4755 "$pkgdir/opt/ciderdarling/$_revision/bin/darling"', recipe)
        for package in ("ffmpeg", "fuse2", "libjpeg-turbo", "libpng", "libx11", "libglvnd", "glibc",
                        "libgcc", "libstdc++", "vulkan-icd-loader"):
            self.assertIn(f"'{package}'", recipe)
        self.assertIn(f"setcap cap_sys_admin=ep /opt/ciderdarling/{REVISION}/libexec/darling/usr/sbin/fseventsd",
                      files["ciderdarling-bin.install"])
        self.assertEqual(subprocess.run(["bash", "-n"], input=recipe, text=True).returncode, 0)

    def test_only_the_arch_build_and_known_libraries(self):
        result, _files = self.render(dict(MANIFEST, flavor="ubuntu24.04"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Arch Linux build", result.stderr)
        result, _files = self.render(dict(MANIFEST, native_libraries=["libunheard.so.1"]))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libunheard.so.1", result.stderr)
        result, _files = self.render(dict(MANIFEST, archive_sha256="not a digest"))
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
