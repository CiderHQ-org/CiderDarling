#!/usr/bin/env python3
"""Download pinned public dependencies and prepare Cider patches; no installation."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    environment = dict(os.environ, GIT_LFS_SKIP_SMUDGE="1", GIT_TERMINAL_PROMPT="0")
    git = ["git", "-c", "credential.helper=", "-c", "core.askPass=/bin/false",
           "-c", "filter.lfs.required=false", "-c", "filter.lfs.smudge=", "-c", "filter.lfs.process="]
    for arguments in (["submodule", "sync", "--recursive"],
                      ["submodule", "update", "--init", "--recursive", "--depth", "1", "--jobs", str(args.jobs)]):
        subprocess.run(git + arguments, cwd=ROOT, env=environment, check=True)
    # Keep optional Swift SDK files as tracked pointer files. A global LFS
    # filter may refer to a missing executable even after a successful clone.
    swift = ROOT / "src/external/swift"
    if (swift / ".git").exists():
        for key, value in (("required", "false"), ("process", ""), ("clean", "cat"), ("smudge", "cat")):
            subprocess.run(["git", "-C", str(swift), "config", "--local", "filter.lfs." + key, value], check=True)
    subprocess.run([sys.executable, str(ROOT / "tools/cider/prepare.py")], check=True)


if __name__ == "__main__":
    main()
