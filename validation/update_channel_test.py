import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('publisher', Path(__file__).resolve().parents[1] / 'tools/cider/publish-update.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class PublishedRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.revision = 'a' * 40
        self.manifests, self.assets = {}, {}
        for flavor in publisher.FLAVORS:
            name = f'ciderdarling-{self.revision}-x86_64-{flavor}'
            data = {'schema_version': 1, 'name': 'CiderDarling', 'architecture': 'x86_64',
                    'source_revision': self.revision, 'source_date': 123, 'flavor': flavor,
                    'install_prefix': '/opt/ciderdarling/' + self.revision,
                    'archive': name + '.tar.xz', 'archive_sha256': 'b' * 64,
                    'patch_manifest_sha256': 'c' * 64, 'archive_bytes': 4096,
                    'min_glibc': '2.39', 'min_glibcxx': '3.4.30', 'min_cxxabi': '1.3.13',
                    'min_libgcc': '3.0', 'native_libraries': ['libcairo.so.2']}
            self.manifests[flavor] = json.dumps(data).encode()
            self.assets[name + '.tar.xz'] = 4096

    def feed(self):
        return publisher.build_feed(self.revision, self.manifests, self.assets, 123, ['d' * 40])

    def test_complete_release_pins_both_archives_and_the_actual_manifest_bytes(self):
        import hashlib
        feed = self.feed()
        self.assertEqual(feed['source_revision'], self.revision)
        self.assertEqual(feed['minimum_cider'], '0.23')
        self.assertEqual(set(feed['builds']), set(publisher.FLAVORS))
        self.assertEqual(feed['builds']['arch']['manifest_sha256'],
                         hashlib.sha256(self.manifests['arch']).hexdigest())

    def test_incomplete_or_mismatched_archives_are_not_advertised(self):
        self.assets.pop(next(iter(self.assets)))
        with self.assertRaises(ValueError):
            self.feed()

    def test_inconsistent_source_or_library_requirements_are_rejected(self):
        original = self.manifests['arch']
        for field, value in [('patch_manifest_sha256', 'e' * 64), ('source_date', 999),
                             ('source_revision', 'f' * 40), ('min_glibc', 'bad'),
                             ('native_libraries', ['/tmp/other.so'])]:
            with self.subTest(field=field):
                data = json.loads(original)
                data[field] = value
                self.manifests['arch'] = json.dumps(data).encode()
                with self.assertRaises(ValueError):
                    self.feed()


if __name__ == '__main__':
    unittest.main()
