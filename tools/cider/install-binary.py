#!/usr/bin/env python3
"""Install a prebuilt CiderDarling archive made by tools/cider/package.py.

  install-binary.py ciderdarling-<revision>-x86_64.tar.xz [--manifest FILE]

The archive is verified against its manifest (SHA-256, revision, host glibc and
libstdc++) before anything is written. It is unpacked with sudo into a hidden
directory under /opt/ciderdarling and renamed into place only when complete, so
an interrupted install never leaves a half-populated version directory. An
existing version directory is never overwritten. Nothing outside
/opt/ciderdarling/<revision> is modified.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

INSTALL_ROOT = Path("/opt/ciderdarling")


def die(message):
    print("error: " + message, file=sys.stderr)
    sys.exit(1)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def version_key(text):
    return tuple(int(part) for part in text.split("."))


def host_glibc():
    libc = ctypes.CDLL(None)
    libc.gnu_get_libc_version.restype = ctypes.c_char_p
    return libc.gnu_get_libc_version().decode()


def host_glibcxx():
    """Highest GLIBCXX version the host's libstdc++ provides, or None."""
    try:
        paths = subprocess.run(["ldconfig", "-p"], check=True, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    best = None
    for line in paths.splitlines():
        if "libstdc++.so.6" in line and "x86-64" in line:
            path = line.split("=>")[-1].strip()
            try:
                data = Path(path).resolve().read_bytes()
            except OSError:
                continue
            for found in re.findall(rb"GLIBCXX_([0-9]+(?:\.[0-9]+)+)", data):
                version = found.decode()
                if best is None or version_key(version) > version_key(best):
                    best = version
    return best


FILE_CAPABILITIES = {"libexec/darling/usr/sbin/fseventsd": "cap_sys_admin=ep"}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--manifest", type=Path,
                        help="manifest JSON (default: ciderdarling-<revision>-x86_64.json next to the archive)")
    args = parser.parse_args()

    if platform.system() != "Linux" or platform.machine() != "x86_64":
        die("CiderDarling binaries are for x86_64 Linux.")
    archive = args.archive.resolve()
    if not archive.is_file():
        die("no such archive: %s" % archive)
    manifest_path = args.manifest or archive.with_name(re.sub(r"\.tar\.xz$", ".json", archive.name))
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, ValueError) as error:
        die("cannot read the manifest %s: %s" % (manifest_path, error))
    revision = manifest.get("source_revision", "")
    if (manifest.get("schema_version") != 1 or manifest.get("name") != "CiderDarling"
            or not re.fullmatch(r"[0-9a-f]{40}", revision)
            or manifest.get("install_prefix") != str(INSTALL_ROOT / revision)
            or manifest.get("archive") != archive.name):
        die("the manifest does not describe this archive.")

    print("Verifying %s ..." % archive.name)
    if sha256(archive) != manifest.get("archive_sha256"):
        die("SHA-256 mismatch; the download is damaged or was modified.")

    glibc, needed = host_glibc(), manifest["min_glibc"]
    if version_key(glibc) < version_key(needed):
        die("this binary needs glibc %s or newer; this system has %s. Build from source instead."
            % (needed, glibc))
    glibcxx = host_glibcxx()
    if glibcxx is not None and version_key(glibcxx) < version_key(manifest["min_glibcxx"]):
        die("this binary needs libstdc++ with GLIBCXX_%s; this system has GLIBCXX_%s. "
            "Update libstdc++ or build from source." % (manifest["min_glibcxx"], glibcxx))

    target = INSTALL_ROOT / revision
    if target.exists():
        die("%s already exists; remove it first if you want to reinstall." % target)
    partial = INSTALL_ROOT / (".partial-" + revision)

    print("Installing into %s (sudo is needed) ..." % target)
    sudo = [] if os.geteuid() == 0 else ["sudo"]
    try:
        subprocess.run(sudo + ["rm", "-rf", "--", str(partial)], check=True)
        subprocess.run(sudo + ["mkdir", "-p", "--", str(partial)], check=True)
        subprocess.run(sudo + ["tar", "-xpJf", str(archive), "--numeric-owner", "--same-owner",
                               "-C", str(partial)], check=True)
        unpacked = partial / revision
        client = unpacked / "bin/darling"
        info = client.stat()
        if not info.st_mode & 0o4000 or info.st_uid != 0:
            die("bin/darling lost its setuid-root bit; the archive was unpacked without privileges.")
        data = json.loads((unpacked / "libexec/darling/usr/share/ciderdarling/runtime.json").read_text())
        if data.get("source_revision") != revision or data.get("patch_manifest_sha256") != manifest["patch_manifest_sha256"]:
            die("the unpacked runtime does not match the manifest.")
        subprocess.run(sudo + ["mv", "--no-target-directory", "--", str(unpacked), str(target)], check=True)
        # File capabilities are not in the archive; `cmake --install` sets these.
        for relative, capabilities in FILE_CAPABILITIES.items():
            if subprocess.run(sudo + ["setcap", capabilities, str(target / relative)], check=False).returncode:
                print("warning: could not set %s on %s" % (capabilities, relative), file=sys.stderr)
    except (subprocess.CalledProcessError, OSError) as error:
        print("error: installation failed: %s" % error, file=sys.stderr)
        sys.exit(1)
    finally:
        subprocess.run(sudo + ["rm", "-rf", "--", str(partial)], check=False)
    print("CiderDarling %s installed in %s" % (revision[:12], target))


if __name__ == "__main__":
    main()
