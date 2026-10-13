import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('profiler', Path(__file__).resolve().parents[1] / 'tools/cider/profile.py')
profiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiler)


class PresentationGapTests(unittest.TestCase):
    def test_interleaved_contexts_do_not_create_false_gaps(self):
        records = [(1, 1000000), (2, 2000000), (1, 17000000), (2, 18000000), (1, 250000000)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'events.jsonl'
            path.write_text(''.join(json.dumps({'category': 'cgl_present', 'pid': 10, 'thread': 11,
                'timestamp_ns': timestamp, 'duration_ns': 100, 'value': context}) + '\n'
                for context, timestamp in records))
            report = profiler.summarize(path)
        self.assertEqual(report['presentation_intervals']['count'], 3)
        self.assertEqual(report['long_presentation_gaps']['count'], 1)
        self.assertEqual(report['long_presentation_gaps']['largest'][0]['interval_ns'], 233000000)
        self.assertEqual(report['long_presentation_gaps']['largest'][0]['context'], 1)


if __name__ == '__main__':
    unittest.main()
