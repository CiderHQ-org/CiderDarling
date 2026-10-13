#!/usr/bin/env python3
"""Test an exact packaged runtime in a disposable user/mount/PID namespace."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/cider'))
import isolate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.getuid() == 0:
        parser.error('Run as an unprivileged build user, never host root')
    if args.output.exists():
        parser.error('Existing smoke reports are preserved')
    manifest = json.loads(args.manifest.read_bytes())
    revision = manifest['source_revision']
    if (not re.fullmatch('[0-9a-f]{40}', revision)
            or manifest['install_prefix'] != f'/opt/ciderdarling/{revision}'
            or Path(manifest['archive']).name != manifest['archive']):
        parser.error('Invalid runtime manifest')
    archive = args.manifest.parent / manifest['archive']
    digest = hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest()
    if digest != manifest['archive_sha256'] or archive.stat().st_size != manifest['archive_bytes']:
        parser.error('The packaged archive does not match the manifest')
    report = {'schema_version': 1, 'source_revision': revision,
              'archive_sha256': digest, 'flavor': manifest['flavor'],
              'coverage': ['math', 'threading', 'network', 'kqueue', 'appkit', 'cgl'],
              'graphics': 'software Mesa under Xvfb; hardware gameplay is separate',
              'passed': False}
    # Keep prefixes short enough for Darling's AF_UNIX sockets. The unpacked
    # setuid executable is owned by the invoking user and is used only inside
    # the disposable namespace, at its original compiled path.
    with tempfile.TemporaryDirectory(prefix='cs-', dir='/tmp') as name:
        stage = Path(name)
        with tarfile.open(archive) as source:
            for member in source:
                parts = Path(member.name).parts
                if (not parts or parts[0] != revision or '..' in parts or member.isdev()
                        or (member.islnk() and (Path(member.linkname).is_absolute()
                                               or '..' in Path(member.linkname).parts))):
                    parser.error('Unsafe runtime archive member')
        subprocess.run(['tar', '-xJf', str(archive.resolve()), '--no-same-owner', '-C', str(stage)], check=True)
        runtime = stage / revision
        installed = json.loads((runtime / 'libexec/darling/usr/share/ciderdarling/runtime.json').read_bytes())
        if (installed.get('source_revision') != revision
                or installed.get('patch_manifest_sha256') != manifest['patch_manifest_sha256']):
            parser.error('Packaged runtime metadata differs from its manifest')
        # namespace_run derives its prefix from directory.parents[2].
        directory = stage / 'runs' / 'smoke' / 'run'
        try:
            status = isolate.namespace_run(runtime, directory, None,
                                           release_mount=Path(manifest['install_prefix']))
            results = json.loads((directory / 'results.json').read_bytes())
            report['results'] = results['results']
            report['passed'] = status == 0 and len(report['results']) == 2
            for path in directory.glob('*.log'):
                print(f'--- {path.name} ---', flush=True)
                print(path.read_text(errors='replace')[-16000:], flush=True)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            report['error'] = str(error)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x') as output:
            json.dump(report, output, indent=2)
            output.write('\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
