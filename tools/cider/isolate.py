#!/usr/bin/env python3
"""Build/stage a private runtime and run fixtures in a disposable namespace."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import doctor

ROOT = Path(__file__).resolve().parents[2]


def run(command, **kwargs):
    print("+ " + " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def cache(build):
    values = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def stage(build, runtime, jobs):
    if runtime in (Path("/"), Path("/usr"), Path("/usr/local")) or not runtime.is_relative_to(ROOT):
        raise ValueError("Development staging must stay inside this checkout")
    marker = runtime / ".ciderdarling-stage"
    expected = str(ROOT) + "\n"
    if runtime.exists() and any(runtime.iterdir()) and (not marker.is_file() or marker.read_text() != expected):
        raise ValueError("Refusing to overwrite an unmanaged runtime directory")
    runtime.mkdir(parents=True, exist_ok=True)
    marker.write_text(expected)
    run(["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         "-DENABLE_METAL=OFF", "-DTARGET_i386=OFF", "-DCIDER_SHUTDOWN_ON_INSTALL=OFF",
         f"-DCMAKE_INSTALL_PREFIX={runtime}"])
    configuration = cache(build)
    if Path(configuration["CMAKE_INSTALL_PREFIX"]).resolve() != runtime or configuration.get("CIDER_SHUTDOWN_ON_INSTALL") != "OFF":
        raise ValueError("Unsafe or mismatched staged-install configuration")
    run(["cmake", "--build", build, "--parallel", jobs])
    environment = dict(os.environ)
    environment.pop("DESTDIR", None)
    run(["cmake", "--install", build], env=environment)
    metadata = json.loads((runtime / "libexec/darling/usr/share/ciderdarling/runtime.json").read_text())
    expected_manifest = hashlib.sha256((ROOT / "patches/cider/series.json").read_bytes()).hexdigest()
    if metadata.get("patch_manifest_sha256") != expected_manifest:
        raise ValueError("Sources changed during the build; rerun staging before executing fixtures")


def compile_fixture(runtime, source, output):
    sysroot = ROOT / "Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
    command = ["clang", "-target", "x86_64-apple-darwin20", "-mmacosx-version-min=11.0",
               "-D__DARWIN_ONLY_UNIX_CONFORMANCE=1", "-DDARLING", "-DTARGET_OS_WASI=0", "-isysroot", sysroot, "-fuse-ld=lld",
               "-I", ROOT / "framework-include",
               f"-Wl,-syslibroot,{runtime / 'libexec/darling'}",
               "-Wno-nullability-completeness", "-Wno-availability", "-Wno-expansion-to-defined",
               "-Wno-deprecated-declarations",
               "-L", runtime / "libexec/darling/usr/lib", "-O2", source, "-o", output]
    if source.suffix == ".m":
        command += ["-F", runtime / "libexec/darling/System/Library/Frameworks", "-framework", "AppKit",
                    "-framework", "Foundation", "-framework", "OpenGL"]
    run(command)


def namespace_run(runtime, directory, vendor):
    metadata = runtime / "libexec/darling/usr/share/ciderdarling/runtime.json"
    data = json.loads(metadata.read_text())
    if data.get("name") != "CiderDarling": raise ValueError("Selected runtime lacks CiderDarling metadata")
    directory.mkdir(parents=True, exist_ok=True)
    fixtures = []
    for name in ("darwin-runtime.c", "darwin-graphics.m"):
        output = directory / Path(name).stem
        compile_fixture(runtime, ROOT / "validation" / name, output)
        fixtures.append(output)
    command = ["unshare", "--user", "--map-root-user", "--mount", "--pid", "--fork", "--kill-child", "--mount-proc",
               sys.executable, ROOT / "tools/cider/prefix-exec.py", "--runtime", runtime,
               "--directory", directory, "--fixtures", *fixtures]
    if vendor: command += ["--egl-vendor", vendor]
    environment = dict(os.environ)
    for key in ("LD_PRELOAD", "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DPREFIX"):
        environment.pop(key, None)
    completed = subprocess.run(list(map(str, command)), env=environment, timeout=180)
    return completed.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--runtime", type=Path, default=ROOT / "build/cider-isolation/runtime")
    parser.add_argument("--stage", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--egl-vendor", type=Path)
    args = parser.parse_args()
    if not args.stage and not args.run: parser.error("Select --stage, --run, or both")
    cgroup = doctor.cgroup_inventory()
    cpus = len(os.sched_getaffinity(0))
    if cgroup["cpu_capacity"] is not None: cpus = min(cpus, max(1, __import__("math").ceil(cgroup["cpu_capacity"])))
    memory = doctor.memory_inventory(cgroup)["effective_available_bytes"]
    jobs = args.jobs if args.jobs is not None else max(1, min(cpus, int(memory * 0.75) // (1024**3)))
    if not 1 <= jobs <= 8192: parser.error("Invalid job count")
    runtime = args.runtime.resolve()
    if args.stage: stage(args.build.resolve(), runtime, jobs)
    if args.run:
        parent = ROOT / "build/cider-isolation/runs"
        parent.mkdir(parents=True, exist_ok=True)
        directory = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
        print(f"Private fixture results: {directory}", flush=True)
        return namespace_run(runtime, directory, args.egl_vendor)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
