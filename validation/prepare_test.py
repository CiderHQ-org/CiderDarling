import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare", ROOT / "tools/cider/prepare.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
SERIES = ROOT / "patches/cider/series.json"


class PreparationTests(unittest.TestCase):
    def fixture(self, destination):
        for entry in json.loads(SERIES.read_text())["files"]:
            data = (ROOT / entry["path"]).read_bytes()
            if prepare.digest(data) == entry["after"]:
                data = prepare.patched_text(data, (SERIES.parent / entry["patch"]).read_text(), True)
            if entry["before"] is None:
                data = None
            self.assertEqual(prepare.digest(data), entry["before"])
            path = destination / entry["path"]
            path.parent.mkdir(parents=True, exist_ok=True)
            if entry["before"] is not None:
                path.write_bytes(data)

    def test_apply_check_idempotence_and_reverse(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.fixture(root)
            total = len(json.loads(SERIES.read_text())["files"])
            self.assertEqual(prepare.prepare(root, SERIES, check=True), total)
            self.assertEqual(prepare.prepare(root, SERIES), total)
            self.assertEqual(prepare.prepare(root, SERIES), 0)
            self.assertEqual(prepare.prepare(root, SERIES, reverse=True), total)
            self.assertEqual(prepare.prepare(root, SERIES, reverse=True), 0)

    def test_unknown_source_rejects_whole_series(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.fixture(root)
            entries = json.loads(SERIES.read_text())["files"]
            last = root / entries[-1]["path"]
            last.write_bytes(last.read_bytes() + b"local edit\n")
            first = root / entries[0]["path"]
            before = first.read_bytes()
            with self.assertRaisesRegex(ValueError, "Unexpected source"):
                prepare.prepare(root, SERIES)
            self.assertEqual(first.read_bytes(), before)
            self.assertTrue(last.read_bytes().endswith(b"local edit\n"))

    def test_source_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.fixture(root)
            first = root / json.loads(SERIES.read_text())["files"][0]["path"]
            saved = first.with_suffix(".saved")
            first.rename(saved)
            first.symlink_to(saved)
            with self.assertRaisesRegex(ValueError, "Unsafe"):
                prepare.prepare(root, SERIES)


if __name__ == "__main__":
    unittest.main()
