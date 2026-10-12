import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools/cider" / (name + ".py"))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


doctor, profile = module("doctor"), module("profile")


class DiagnosticsTests(unittest.TestCase):
    def test_parent_memory_pressure_and_cpu_quotas(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            child = root / "parent/child"
            child.mkdir(parents=True)
            for directory, limit, used, quota in ((root, "max", 1, "max 100000"),
                    (root / "parent", "1000", 900, "75000 100000"),
                    (child, "800", 10, "200000 100000")):
                (directory / "memory.max").write_text(limit)
                (directory / "memory.current").write_text(str(used))
                (directory / "cpu.max").write_text(quota)
            real_read = doctor.read
            def read(path, *arguments):
                if str(path) == "/proc/self/cgroup": return "0::/parent/child"
                return real_read(path, *arguments)
            with patch.object(doctor, "read", side_effect=read):
                group = doctor.cgroup_inventory(root)
            self.assertEqual(group["memory_limit_bytes"], 800)
            self.assertEqual(group["memory_headroom_bytes"], 100)
            self.assertEqual(group["cpu_capacity"], 0.75)

    def test_small_sample_tails_and_context_intervals(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "events.jsonl"
            records = []
            for context, times in ((1, (10, 30)), (2, (100, 140))):
                for timestamp in times:
                    records.append({"category": "cgl_present", "pid": 1, "thread": 2,
                                    "timestamp_ns": timestamp, "duration_ns": timestamp, "value": context})
            path.write_text("".join(json.dumps(record) + "\n" for record in records) + "invalid\n")
            report = profile.summarize(path)
            self.assertEqual(report["presentation_intervals"]["count"], 2)
            self.assertEqual(report["presentation_intervals"]["p95_ns"], 40)
            self.assertEqual(report["rejected_records"], 1)
            self.assertGreaterEqual(profile.percentiles([20, 30])["p95_ns"], profile.percentiles([20, 30])["p50_ns"])


if __name__ == "__main__":
    unittest.main()
