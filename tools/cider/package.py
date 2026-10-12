#!/usr/bin/env python3
"""Build CiderDarling and package it as a relocation-free binary archive.

The archive holds the tree that `cmake --install` produces for the prefix
/opt/ciderdarling/<revision>, with root ownership and the setuid darling client,
so it can be unpacked on another x86_64 Linux host instead of compiling there.
Nothing is installed on this machine and nothing is uploaded.

Outputs, in --output (with --flavor, the names end in -x86_64-<flavor>):
  ciderdarling-<revision>-x86_64.tar.xz
  ciderdarling-<revision>-x86_64.tar.xz.sha256
  ciderdarling-<revision>-x86_64.json     (revision, hashes, host requirements)

Darling's bridges to host libraries (usr/lib/native) name the exact library
versions of the build host, so an archive suits hosts with the same library
generation as its build host (the manifest lists them): CI builds one flavor
per distribution family.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
INSTALL_ROOT = Path("/opt/ciderdarling")


def run(command, **kwargs):
    print("+ " + " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def output(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, text=True,
                          stdout=subprocess.PIPE, **kwargs).stdout


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def version_key(text):
    return tuple(int(part) for part in text.split("."))


def host_requirements(stage):
    """Highest glibc/libstdc++ symbol versions and shared libraries the ELF files need."""
    newest = {"GLIBC": "0", "GLIBCXX": "0", "CXXABI": "0", "GCC": "0"}
    needed = set()
    pattern = re.compile(r"@(GLIBC|GLIBCXX|CXXABI|GCC)_([0-9][0-9.]*)")
    count = 0
    for path in stage.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        with open(path, "rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        count += 1
        dynamic = output(["readelf", "-d", "-W", path], stderr=subprocess.DEVNULL)
        needed.update(re.findall(r"Shared library: \[([^\]]+)\]", dynamic))
        symbols = output(["readelf", "--dyn-syms", "-W", path], stderr=subprocess.DEVNULL)
        for family, version in pattern.findall(symbols):
            if version_key(version) > version_key(newest[family]):
                newest[family] = version
    return {"elf_files": count,
            "min_glibc": newest["GLIBC"], "min_glibcxx": newest["GLIBCXX"],
            "min_cxxabi": newest["CXXABI"], "min_libgcc": newest["GCC"],
            "shared_libraries": sorted(needed)}


def native_libraries(runtime):
    """Host libraries the native bridges load, by exact soname."""
    names = set()
    pattern = re.compile(rb"lib[A-Za-z0-9_+-]+\.so(?:\.[0-9]+)*")
    for path in (runtime / "libexec/darling/usr/lib/native").glob("*.dylib"):
        names.update(match.decode() for match in pattern.findall(path.read_bytes()))
    return sorted(names)


def build_host():
    try:
        for line in Path("/etc/os-release").read_text().splitlines():
            if line.startswith("PRETTY_NAME="):
                return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return "unknown"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-release",
                        help="CMake build directory (created if missing; default: build-release)")
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--skip-build", action="store_true",
                        help="package an already built --build-dir, configured for this revision's prefix")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="package with uncommitted changes (the revision in the archive will not describe it)")
    parser.add_argument("--flavor", help="distribution family the build host represents, e.g. arch or ubuntu24.04")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 128:
        parser.error("--jobs must be between 1 and 128")
    if args.flavor is not None and not re.fullmatch(r"[a-z0-9][a-z0-9.]{0,31}", args.flavor):
        parser.error("--flavor must be lowercase letters, digits and dots")

    revision = output(["git", "rev-parse", "HEAD"], cwd=ROOT).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        sys.exit("Cannot determine the source revision; build from a git checkout.")
    dirty = output(["git", "status", "--porcelain", "--ignore-submodules=all"], cwd=ROOT).strip()
    if dirty and not args.allow_dirty:
        sys.exit("Uncommitted changes would not be described by revision %s. Commit them or pass --allow-dirty.\n%s"
                 % (revision[:12], dirty))
    epoch = output(["git", "show", "-s", "--format=%ct", "HEAD"], cwd=ROOT).strip()
    prefix = INSTALL_ROOT / revision
    build = args.build_dir.resolve()
    name = "ciderdarling-%s-x86_64" % revision + ("-" + args.flavor if args.flavor else "")

    if not args.skip_build:
        run([sys.executable, ROOT / "tools/cider/prepare.py"])
        run(["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
             "-DENABLE_METAL=OFF", "-DTARGET_i386=OFF", "-DCIDER_SHUTDOWN_ON_INSTALL=OFF",
             f"-DCMAKE_INSTALL_PREFIX={prefix}"])
        run(["cmake", "--build", build, "--parallel", args.jobs])
    configured = ""
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith("CMAKE_INSTALL_PREFIX:"):
            configured = line.split("=", 1)[1]
    if configured != str(prefix):
        sys.exit("The build is configured for %s, not %s." % (configured or "no prefix", prefix))

    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=".package-", dir=args.output))
    try:
        stage = work / "stage"
        environment = dict(os.environ, DESTDIR=str(stage))
        run(["cmake", "--install", build], env=environment)
        runtime = stage / str(prefix).lstrip("/")
        metadata = json.loads((runtime / "libexec/darling/usr/share/ciderdarling/runtime.json").read_text())
        series = sha256(ROOT / "patches/cider/series.json")
        if metadata.get("source_revision") != revision or metadata.get("patch_manifest_sha256") != series:
            sys.exit("The build does not match this checkout; reconfigure it (without --skip-build).")
        client = runtime / "bin/darling"
        if not client.is_file() or not client.stat().st_mode & 0o4000:
            sys.exit("bin/darling is missing or not setuid in the staged install.")

        requirements = host_requirements(runtime)
        archive = args.output / (name + ".tar.xz")
        print("+ tar | xz ->", archive, flush=True)
        tar = subprocess.Popen(
            ["tar", "--sort=name", "--owner=0", "--group=0", "--numeric-owner", "--mtime=@" + epoch,
             "-C", str(runtime.parent), "-cf", "-", revision], stdout=subprocess.PIPE)
        with open(archive, "wb") as stream:
            xz = subprocess.run(["xz", "-T0", "-6", "-c"], stdin=tar.stdout, stdout=stream)
        tar.stdout.close()
        if tar.wait() or xz.returncode:
            archive.unlink(missing_ok=True)
            sys.exit("Archive creation failed.")

        listing = output(["tar", "-tvJf", archive, revision + "/bin/darling"])
        if not re.match(r"-rws\S+ 0/0 ", listing):
            archive.unlink(missing_ok=True)
            sys.exit("The archive does not keep darling setuid root: " + listing.strip())

        digest = sha256(archive)
        Path(str(archive) + ".sha256").write_text("%s  %s\n" % (digest, archive.name))
        manifest = {
            "schema_version": 1, "name": "CiderDarling", "architecture": "x86_64",
            "source_revision": revision, "patch_manifest_sha256": series,
            "source_date": int(epoch), "flavor": args.flavor, "build_host": build_host(),
            "install_prefix": str(prefix), "archive": archive.name,
            "archive_sha256": digest, "archive_bytes": archive.stat().st_size,
            "installed_bytes": sum(path.stat().st_size for path in runtime.rglob("*")
                                   if path.is_file() and not path.is_symlink()),
            "build_type": "RelWithDebInfo", "metal": False, **requirements,
            "native_libraries": native_libraries(runtime),
        }
        (args.output / (name + ".json")).write_text(json.dumps(manifest, indent=2) + "\n")
        print(json.dumps(manifest, indent=2))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
