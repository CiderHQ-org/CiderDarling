#!/usr/bin/env python3
"""Apply the reviewed Cider source series without fuzz or unknown-source edits."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SERIES = ROOT / "patches/cider/series.json"


def digest(data):
    return hashlib.sha256(data).hexdigest() if data is not None else None


def patched_text(original, patch, reverse=False):
    """Interpret one complete unified file diff, checking every context line."""
    source = (original or b"").decode("utf-8").splitlines(keepends=True)
    lines = patch.splitlines(keepends=True)
    output, cursor, index = [], 0, 2
    while index < len(lines):
        match = re.fullmatch(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@[^\n]*\n", lines[index])
        if not match:
            raise ValueError("Invalid unified diff hunk")
        old_start, old_count, new_start, new_count = match.groups()
        start = int(new_start if reverse else old_start)
        count = int((new_count if reverse else old_count) or "1")
        start = start - 1 if start else 0
        if start < cursor or start > len(source):
            raise ValueError("Overlapping or misplaced diff hunk")
        output.extend(source[cursor:start])
        cursor, consumed, produced = start, 0, 0
        index += 1
        while index < len(lines) and not lines[index].startswith("@@ "):
            line = lines[index]
            marker, content = line[:1], line[1:]
            if marker not in (" ", "+", "-"):
                raise ValueError("Unsupported unified diff line")
            if reverse and marker != " ":
                marker = "+" if marker == "-" else "-"
            if marker in (" ", "-"):
                if cursor >= len(source) or source[cursor] != content:
                    raise ValueError("Source does not match diff context")
                cursor += 1
                consumed += 1
            if marker in (" ", "+"):
                output.append(content)
                produced += 1
            index += 1
        expected_output = int((old_count if reverse else new_count) or "1")
        if consumed != count or produced != expected_output:
            raise ValueError("Invalid unified diff line counts")
    output.extend(source[cursor:])
    return "".join(output).encode("utf-8")


def prepare(root=ROOT, series=SERIES, *, check=False, reverse=False):
    root, series = Path(root).resolve(), Path(series)
    manifest = json.loads(series.read_text())
    planned, seen = [], set()
    for entry in manifest["files"]:
        relative = entry["path"]
        path = root / relative
        if relative in seen or path.is_symlink() or not path.resolve().is_relative_to(root):
            raise ValueError(f"Unsafe or duplicate source path: {relative}")
        seen.add(relative)
        patch_path = series.parent / entry["patch"]
        patch_data = patch_path.read_bytes()
        if digest(patch_data) != entry["patch_sha256"]:
            raise ValueError(f"Patch digest mismatch: {entry['patch']}")
        before, after = entry["before"], entry["after"]
        expected, target = (after, before) if reverse else (before, after)
        data = path.read_bytes() if path.exists() else None
        current = digest(data)
        if current == target:
            continue
        if current != expected:
            raise ValueError(f"Unexpected source in {relative}; no files changed. Initialize pinned submodules or preserve/rebase your local edits first.")
        replacement = patched_text(data, patch_data.decode("utf-8"), reverse)
        if target is None:
            if replacement:
                raise ValueError(f"Nonempty deletion: {relative}")
            replacement = None
        if digest(replacement) != target:
            raise ValueError(f"Patch result digest mismatch: {relative}")
        planned.append((path, data, replacement))
    if not check:
        # Validate the entire series before any edit, including already-applied
        # files. Individual replacements are atomic and reruns are idempotent.
        for path, previous, replacement in planned:
            current = path.read_bytes() if path.exists() else None
            if current != previous or path.is_symlink():
                raise ValueError(f"Source changed during preparation: {path}")
        for path, previous, replacement in planned:
            if replacement is None:
                path.unlink()
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            mode = path.stat().st_mode & 0o777 if previous is not None else 0o644
            fd, name = tempfile.mkstemp(prefix=".cider-", dir=path.parent)
            try:
                with os.fdopen(fd, "wb") as output:
                    os.fchmod(output.fileno(), mode)
                    output.write(replacement)
                os.replace(name, path)
            finally:
                Path(name).unlink(missing_ok=True)
    return len(planned)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--reverse", action="store_true")
    args = parser.parse_args()
    try:
        count = prepare(args.root, check=args.check, reverse=args.reverse)
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, f"Cider patches: {error}\n")
    action = "ready" if args.check else "restored" if args.reverse else "applied"
    print(f"Cider patches: {count} source files {action}; remaining files already match")


if __name__ == "__main__":
    main()
