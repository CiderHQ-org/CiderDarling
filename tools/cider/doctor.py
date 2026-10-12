#!/usr/bin/env python3
"""Read-only host/runtime diagnostics; graphics checks run in a child process."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import subprocess


def read(path, limit=256 * 1024):
    try:
        with Path(path).open() as source:
            return source.read(limit).strip()
    except (OSError, UnicodeError):
        return ""


def gpu_inventory(drm=Path("/sys/class/drm")):
    adapters = {}
    for node in sorted(drm.glob("*")):
        if not re.fullmatch(r"card\d+|renderD\d+", node.name):
            continue
        device = node / "device"
        properties = dict(line.split("=", 1) for line in read(device / "uevent").splitlines() if "=" in line)
        pci = properties.get("PCI_SLOT_NAME")
        if not pci:
            continue
        try:
            vendor = int(read(device / "vendor"), 16)
            product = int(read(device / "device"), 16)
        except ValueError:
            continue
        item = adapters.setdefault(pci, {"pci": pci, "vendor": vendor, "device": product,
            "vendor_name": {0x10de: "NVIDIA", 0x1002: "AMD", 0x8086: "Intel"}.get(vendor, "Other"),
            "driver": (device / "driver").resolve().name if (device / "driver").exists() else None,
            "render_nodes": []})
        if node.name.startswith("renderD"):
            item["render_nodes"].append("/dev/dri/" + node.name)
        capacity = read(device / "mem_info_vram_total")
        if capacity.isdecimal():
            item["dedicated_vram_bytes"] = int(capacity)
    return list(adapters.values())


def cpu_inventory():
    text = read("/proc/cpuinfo", 4 * 1024 * 1024)
    models, flags = set(), []
    for line in text.splitlines():
        key, _, value = line.partition(":")
        if key.strip() == "model name": models.add(value.strip())
        if key.strip() == "flags": flags.append(set(value.split()))
    try:
        allowed = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        allowed = os.cpu_count()
    return {"architecture": platform.machine(), "models": sorted(models),
            "logical_cpus": os.cpu_count(), "allowed_cpus": allowed,
            "common_features": sorted(set.intersection(*flags)) if flags else []}


def memory_inventory():
    values = {}
    for line in read("/proc/meminfo").splitlines():
        key, _, value = line.partition(":")
        if key in ("MemTotal", "MemAvailable", "SwapTotal"):
            try: values[key + "_bytes"] = int(value.split()[0]) * 1024
            except (ValueError, IndexError): pass
    limit = read("/sys/fs/cgroup/memory.max")
    values["exposed_cgroup_limit_bytes"] = int(limit) if limit.isdecimal() else None
    return values


def graphics_probe(program, vendor=None):
    environment = dict(os.environ)
    # This is a Linux host executable, so do not inherit guest loader shims.
    environment.pop("LD_PRELOAD", None)
    if vendor:
        environment["__EGL_VENDOR_LIBRARY_FILENAMES"] = str(vendor)
    try:
        result = subprocess.run([str(program)], capture_output=True, text=True, timeout=20, env=environment)
        data = json.loads(result.stdout)
        if not isinstance(data, dict): raise ValueError("Unexpected probe result")
        data["exit_status"] = result.returncode
        data["ok"] = bool(data.get("ok") and result.returncode == 0)
        renderer = str(data.get("renderer", "")).lower()
        data["software_renderer"] = any(name in renderer for name in
            ("llvmpipe", "lavapipe", "softpipe", "swiftshader", "software rasterizer"))
        data["hardware_usable"] = data["ok"] and not data["software_renderer"]
        if vendor: data["egl_vendor_manifest"] = str(vendor)
        if result.stderr: data["diagnostics"] = result.stderr[-2048:]
        return data
    except (OSError, subprocess.TimeoutExpired, ValueError) as error:
        return {"ok": False, "error": str(error)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Print machine-readable output")
    parser.add_argument("--graphics", action="store_true", help="Run an offscreen host rendering/share check")
    parser.add_argument("--probe", type=Path, help="Path to the built cider-gl-probe executable")
    parser.add_argument("--egl-vendor", type=Path, help="Test one explicit EGL vendor manifest in the child only")
    parser.add_argument("--sysroot", type=Path, help="Darling runtime root containing usr/share/ciderdarling/runtime.json")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    prefix = Path(__file__).resolve().parents[1]
    runtime_files = [args.sysroot / "usr/share/ciderdarling/runtime.json"] if args.sysroot else [
        source / "build/cider-runtime.json",
        prefix / "libexec/darling/usr/share/ciderdarling/runtime.json",
        Path("/usr/local/libexec/darling/usr/share/ciderdarling/runtime.json")]
    runtime = None
    metadata_kind = None
    for path in runtime_files:
        try:
            runtime = json.loads(read(path))
            metadata_kind = "source_build" if path == source / "build/cider-runtime.json" else "selected_sysroot"
            break
        except ValueError:
            pass
    runtime_dir = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    report = {"schema_version": 1, "system": {"kernel": platform.release(),
        "distribution": read("/etc/os-release"), "flatpak": Path("/.flatpak-info").exists()},
        "cpu": cpu_inventory(), "memory": memory_inventory(), "gpus": gpu_inventory(),
        "desktop": {"x11_environment": bool(os.environ.get("DISPLAY")),
                    "wayland_environment": bool(os.environ.get("WAYLAND_DISPLAY"))},
        "audio": {"pipewire_socket_present": (runtime_dir / "pipewire-0").exists(),
                  "pulse_socket_present": (runtime_dir / "pulse/native").exists()},
        "runtime": {"kind": metadata_kind, "data": runtime} if runtime else None, "graphics": None}
    if args.graphics:
        probes = [args.probe] if args.probe else [source / "build/tools/cider/cider-gl-probe",
                  prefix / "libexec/ciderdarling/cider-gl-probe"]
        executable = next((path for path in probes if path and path.is_file()), None)
        report["graphics"] = graphics_probe(executable, args.egl_vendor) if executable else {
            "ok": False, "error": "Build cider-gl-probe or pass --probe PATH"}
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print("CiderDarling host check")
        print(f"CPU: {report['cpu']['architecture']}, {report['cpu']['allowed_cpus']} available logical CPUs")
        for gpu in report["gpus"]:
            print(f"GPU: {gpu['vendor_name']} {gpu['device']:04x} at {gpu['pci']} ({gpu['driver'] or 'driver unknown'})")
        print("Runtime metadata: " + (metadata_kind if runtime else "unavailable"))
        if args.graphics: print("Host graphics: " + json.dumps(report["graphics"]))
        print("Use --json for the complete report. Host checks do not establish gameplay compatibility.")
    if args.graphics and not report["graphics"].get("ok"): return 1
    if args.graphics and report["graphics"].get("software_renderer"): return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
