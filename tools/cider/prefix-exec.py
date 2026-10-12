#!/usr/bin/env python3
"""Internal fixture runner; requires an isolated user and PID namespace."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--fixtures", nargs="+", required=True, type=Path)
    parser.add_argument("--egl-vendor", type=Path)
    args = parser.parse_args()
    if os.getpid() != 1 or os.getuid() != 0: parser.error("Run through isolate.py in a private namespace")
    mapping = Path("/proc/self/uid_map").read_text().split()
    if len(mapping) != 3 or mapping[0] != "0" or mapping[2] != "1" or mapping[1] == "0":
        parser.error("Expected one unprivileged host-user mapping")
    directory = args.directory.resolve()
    # AF_UNIX socket names include the prefix; keep their path below 108 bytes.
    prefix = directory.parents[2] / ("p-" + directory.name)
    if prefix.exists(): parser.error("Fixture prefixes must be fresh")
    client = args.runtime.resolve() / "bin/darling"
    environment = dict(os.environ, DPREFIX=str(prefix), DARLING_NOOVERLAYFS="1")
    environment["PATH"] = str(client.parent) + os.pathsep + environment.get("PATH", "")
    environment["LD_LIBRARY_PATH"] = str(args.runtime / "libexec/darling") + os.pathsep + str(args.runtime / "lib")
    for key in ("LD_PRELOAD", "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
        environment.pop(key, None)
    if args.egl_vendor: environment["__EGL_VENDOR_LIBRARY_FILENAMES"] = str(args.egl_vendor)
    environment["CIDER_TRACE_FILE"] = "/Volumes/SystemRoot" + str(directory / "events.jsonl")
    results = []
    try:
        for fixture in args.fixtures:
            with (directory / (fixture.name + ".log")).open("x") as log:
                try:
                    result = subprocess.run([str(client), "shell", "/Volumes/SystemRoot" + str(fixture.resolve())],
                                            env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=60)
                    results.append({"fixture": fixture.name, "returncode": result.returncode})
                except subprocess.TimeoutExpired:
                    results.append({"fixture": fixture.name, "error": "Fixture/startup timed out"})
    finally:
        if prefix.exists():
            try:
                with (directory / "shutdown.log").open("x") as log:
                    subprocess.run([str(client), "shutdown"], env=environment, stdout=log,
                                   stderr=subprocess.STDOUT, timeout=15)
            except (OSError, subprocess.TimeoutExpired): pass
    report = {"runtime": str(args.runtime), "prefix": str(prefix), "results": results}
    (directory / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)
    return 0 if all(item.get("returncode") == 0 for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
