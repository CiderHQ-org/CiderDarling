#!/usr/bin/env python3
"""Promote a complete CI binary release to Cider's automatic update channel."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = 'CiderHQ-org/CiderDarling'
CHANNEL_TAG = 'runtime-updates'
FLAVORS = ('arch', 'ubuntu24.04')


def command(*arguments, **kwargs):
    return subprocess.run(list(map(str, arguments)), check=True, text=True,
                          capture_output=True, **kwargs).stdout


def git(*arguments):
    return command('git', '-C', ROOT, *arguments).strip()


def ancestor(older, newer):
    return subprocess.run(['git', '-C', str(ROOT), 'merge-base', '--is-ancestor', older, newer],
                          capture_output=True).returncode == 0


def release(tag):
    result = subprocess.run(['gh', 'api', f'repos/{REPOSITORY}/releases/tags/{tag}'],
                            text=True, capture_output=True)
    if result.returncode and '404' in result.stderr:
        return None
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return json.loads(result.stdout)


def download(tag, name, directory):
    directory.mkdir(parents=True, exist_ok=True)
    command('gh', 'release', 'download', tag, '--repo', REPOSITORY,
            '--pattern', name, '--dir', directory)
    file = directory / name
    if file.stat().st_size > 1 << 20:
        raise ValueError(f'{name} is too large')
    return file


def build_feed(revision, manifests, assets, source_date, supersedes):
    """Use only the exact immutable archives and manifests of this revision."""
    series, builds = set(), {}
    for flavor in FLAVORS:
        raw = manifests[flavor]
        data = json.loads(raw)
        stem = f'ciderdarling-{revision}-x86_64-{flavor}'
        archive = stem + '.tar.xz'
        if (data.get('schema_version') != 1 or data.get('name') != 'CiderDarling'
                or data.get('architecture') != 'x86_64' or data.get('source_revision') != revision
                or data.get('source_date') != source_date or data.get('flavor') != flavor
                or data.get('archive') != archive
                or data.get('install_prefix') != f'/opt/ciderdarling/{revision}'
                or not re.fullmatch('[0-9a-f]{64}', data.get('archive_sha256', ''))
                or not re.fullmatch('[0-9a-f]{64}', data.get('patch_manifest_sha256', ''))
                or not isinstance(data.get('archive_bytes'), int) or data['archive_bytes'] <= 0
                or assets.get(archive) != data['archive_bytes']):
            raise ValueError(f'{flavor} does not describe the published archive')
        for field in ('min_glibc', 'min_glibcxx', 'min_cxxabi', 'min_libgcc'):
            if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*', data.get(field, '')):
                raise ValueError(f'Invalid {field}')
        libraries = data.get('native_libraries')
        if (not isinstance(libraries, list) or len(libraries) > 256
                or any(not isinstance(name, str) or not re.fullmatch(r'lib[A-Za-z0-9_+.-]+\.so(?:\.[0-9]+)*', name)
                       for name in libraries)):
            raise ValueError('Invalid native libraries')
        series.add(data['patch_manifest_sha256'])
        builds[flavor] = {key: data[key] for key in ('archive', 'archive_sha256', 'archive_bytes',
            'min_glibc', 'min_glibcxx', 'min_cxxabi', 'min_libgcc', 'native_libraries')}
        builds[flavor].update(manifest=stem + '.json', manifest_sha256=hashlib.sha256(raw).hexdigest())
    if len(series) != 1:
        raise ValueError('The builds disagree about the source patches')
    return {'schema_version': 1, 'name': 'CiderDarling', 'architecture': 'x86_64',
            'channel': 'stable', 'source_revision': revision, 'source_date': source_date,
            'patch_manifest_sha256': series.pop(), 'minimum_cider': '0.23',
            'release_url': f'https://github.com/{REPOSITORY}/releases/tag/bin-{revision[:12]}',
            'supersedes': supersedes, 'builds': builds}


def validate_smoke(report, revision, flavor, digest):
    if (not isinstance(report, dict) or report.get('schema_version') != 1
            or report.get('source_revision') != revision or report.get('flavor') != flavor
            or report.get('archive_sha256') != digest or report.get('passed') is not True
            or report.get('coverage') != ['math', 'threading', 'network', 'kqueue', 'appkit', 'cgl']):
        raise ValueError(f'{flavor} binary smoke gate did not pass for this archive')
    results = report.get('results')
    if (not isinstance(results, list) or len(results) != 2
            or {item.get('fixture') for item in results} != {'darwin-runtime', 'darwin-graphics'}
            or any(item.get('returncode') != 0 or item.get('error') for item in results)):
        raise ValueError(f'{flavor} binary smoke results are incomplete')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('revision')
    args = parser.parse_args()
    revision = args.revision
    if not re.fullmatch('[0-9a-f]{40}', revision) or not ancestor(revision, 'origin/main'):
        parser.error('The revision must belong to main')
    fixes = git('show', f'{revision}:tools/cider/runtime.json.in')
    # Builds preceding the observed exp10 freeze repair remain archived, but
    # are not advertised to users as a new stable runtime.
    if '"libm_builtin_recursion"' not in fixes:
        print('Keeping this older build out of the automatic update channel.')
        return
    tag = 'bin-' + revision[:12]
    record = release(tag)
    if record is None:
        print('No published binary release; nothing to promote.')
        return
    if record['draft'] or record['prerelease']:
        raise ValueError('Only a published stable binary release can be promoted')
    target = json.loads(command('gh', 'api', f'repos/{REPOSITORY}/commits/{tag}'))['sha']
    if target != revision:
        raise ValueError('The release tag names another source revision')
    assets = {asset['name']: asset['size'] for asset in record['assets']}
    with tempfile.TemporaryDirectory(prefix='cider-update-') as directory:
        work = Path(directory)
        manifests = {}
        for flavor in FLAVORS:
            stem = f'ciderdarling-{revision}-x86_64-{flavor}'
            for suffix in ('.json', '.tar.xz', '.tar.xz.sha256'):
                if stem + suffix not in assets:
                    raise ValueError(f'The release is missing {stem + suffix}')
            raw = download(tag, stem + '.json', work).read_bytes()
            checksum = download(tag, stem + '.tar.xz.sha256', work).read_text().split()
            data = json.loads(raw)
            if checksum != [data.get('archive_sha256'), stem + '.tar.xz']:
                raise ValueError('Archive checksum and manifest disagree')
            smoke = f'runtime-smoke-{flavor}.json'
            if smoke not in assets:
                raise ValueError(f'The release is missing {smoke}')
            validate_smoke(json.loads(download(tag, smoke, work).read_bytes()),
                           revision, flavor, data['archive_sha256'])
            manifests[flavor] = raw
        feed = build_feed(revision, manifests, assets, int(git('show', '-s', '--format=%ct', revision)),
                          git('rev-list', '--max-count=129', revision).splitlines()[1:])
        channel = release(CHANNEL_TAG)
        if channel is not None:
            current = json.loads(download(CHANNEL_TAG, 'stable.json', work / 'current').read_bytes())
            prior = current.get('source_revision')
            if not isinstance(prior, str) or not re.fullmatch('[0-9a-f]{40}', prior):
                raise ValueError('Invalid existing update channel')
            if prior != revision and not ancestor(prior, revision):
                print('A newer or different build is already published; keeping it.')
                return
        raw = (json.dumps(feed, sort_keys=True, indent=2) + '\n').encode()
        immutable = work / 'ciderdarling-update.json'
        immutable.write_bytes(raw)
        if immutable.name in assets:
            old = download(tag, immutable.name, work / 'immutable')
            if old.read_bytes() != raw:
                raise ValueError('Refusing to replace an immutable release feed')
        else:
            command('gh', 'release', 'upload', tag, immutable, '--repo', REPOSITORY)
        stable = work / 'stable.json'
        stable.write_bytes(raw)
        if channel is None:
            command('gh', 'release', 'create', CHANNEL_TAG, stable, '--repo', REPOSITORY,
                    '--target', revision, '--latest=false', '--title', 'CiderDarling automatic updates',
                    '--notes', 'Cider checks stable.json for complete, checksum-verified runtime releases. '
                    'Install an offered update from Settings → CiderDarling in the launcher.')
        else:
            command('gh', 'release', 'upload', CHANNEL_TAG, stable, '--clobber', '--repo', REPOSITORY)
        print(f'Published CiderDarling {revision[:12]} to the automatic update channel.')


if __name__ == '__main__':
    main()
