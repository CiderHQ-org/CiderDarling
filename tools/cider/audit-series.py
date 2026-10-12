#!/usr/bin/env python3
"""Verify pinned originals, patch results and coverage of modified dependencies."""
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prepare", ROOT / "tools/cider/prepare.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


def git(directory, *arguments):
    return subprocess.run(["git", "-C", str(directory), *arguments], capture_output=True, check=True).stdout


def main():
    manifest = json.loads(prepare.SERIES.read_text())
    covered, repositories = set(), set()
    for entry in manifest["files"]:
        source = ROOT / entry["path"]
        repository = Path(git(source.parent, "rev-parse", "--show-toplevel").decode().strip())
        repositories.add(repository)
        relative = source.relative_to(repository).as_posix()
        covered.add((repository, relative))
        baseline = None if entry["before"] is None else git(repository, "show", f"HEAD:{relative}")
        if prepare.digest(baseline) != entry["before"]:
            raise ValueError(f"Dependency pin differs for {entry['path']}")
        patch = (prepare.SERIES.parent / entry["patch"]).read_text()
        if prepare.digest(prepare.patched_text(baseline, patch)) != entry["after"]:
            raise ValueError(f"Patch does not reproduce {entry['path']}")
    stage = git(ROOT, "ls-files", "--stage", "src/external").decode().splitlines()
    for line in stage:
        if line.startswith("160000 "):
            directory = ROOT / line.split("\t", 1)[1]
            if (directory / ".git").exists(): repositories.add(directory)
    def inspect(repository):
        tracked = git(repository, "diff", "--name-only", "HEAD", "--").decode().splitlines()
        added = git(repository, "ls-files", "--others", "--exclude-standard").decode().splitlines()
        unexpected = []
        for path in tracked + added:
            if (repository, path) in covered: continue
            nested = repository / path
            if nested in repositories:
                entry = git(repository, "ls-tree", "HEAD", "--", path).decode().split()
                if entry and entry[0] == "160000" and git(nested, "rev-parse", "HEAD").decode().strip() == entry[2]:
                    continue
            unexpected.append((repository, path))
        return unexpected
    unexpected = []
    with ThreadPoolExecutor(max_workers=8) as workers:
        for differences in workers.map(inspect, sorted(repositories)): unexpected.extend(differences)
    if unexpected:
        for repository, path in unexpected: print(f"Unrecorded dependency change: {repository.relative_to(ROOT)}/{path}")
        return 1
    prepare.prepare(check=True)
    print(f"PASS {len(manifest['files'])} patches reproduce their pinned sources and cover dependency edits")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
