#!/usr/bin/env python3
"""Read-only host/runtime diagnostics; graphics checks run in a child process."""
import argparse
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
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
            "render_nodes": [], "card_nodes": []})
        if node.name.startswith("renderD"):
            item["render_nodes"].append("/dev/dri/" + node.name)
        else:
            item["card_nodes"].append("/dev/dri/" + node.name)
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


def cgroup_inventory(root=Path("/sys/fs/cgroup")):
    root = Path(root)
    membership = next((line[3:] for line in read("/proc/self/cgroup").splitlines() if line.startswith("0::")), "/")
    candidate = root / membership.lstrip("/")
    current = candidate if candidate.is_dir() else root
    memory_limits, memory_headroom, cpu_limits, pid_limits = [], [], [], []
    directory = current
    while directory.is_relative_to(root):
        maximum = read(directory / "memory.max")
        if maximum.isdecimal():
            memory_limits.append(int(maximum))
            usage = read(directory / "memory.current")
            if usage.isdecimal(): memory_headroom.append(max(0, int(maximum) - int(usage)))
        maximum = read(directory / "pids.max")
        if maximum.isdecimal(): pid_limits.append(int(maximum))
        quota = read(directory / "cpu.max").split()
        if len(quota) == 2 and all(value.isdecimal() for value in quota) and int(quota[1]):
            cpu_limits.append(int(quota[0]) / int(quota[1]))
        if directory == root: break
        directory = directory.parent
    number = lambda name: int(value) if (value := read(current / name)).isdecimal() else None
    return {"memory_limit_bytes": min(memory_limits) if memory_limits else None,
            "memory_headroom_bytes": min(memory_headroom) if memory_headroom else None,
            "memory_current_bytes": number("memory.current"),
            "cpu_capacity": min(cpu_limits) if cpu_limits else None,
            "pids_limit": min(pid_limits) if pid_limits else None, "pids_current": number("pids.current"),
            "memory_events": read(current / "memory.events"), "cpu_stat": read(current / "cpu.stat")}


def memory_inventory(cgroup=None):
    values = {}
    for line in read("/proc/meminfo").splitlines():
        key, _, value = line.partition(":")
        if key in ("MemTotal", "MemAvailable", "SwapTotal"):
            try: values[key + "_bytes"] = int(value.split()[0]) * 1024
            except (ValueError, IndexError): pass
    cgroup = cgroup or cgroup_inventory()
    values["exposed_cgroup_limit_bytes"] = cgroup["memory_limit_bytes"]
    available = values.get("MemAvailable_bytes", 0)
    if cgroup["memory_headroom_bytes"] is not None:
        available = min(available, cgroup["memory_headroom_bytes"])
    values["effective_available_bytes"] = available
    return values


def graphics_probe(program, vendor=None, *, device=None, frames=120, cycles=16, samples=0, affinity=None, memory_limit=None):
    environment = dict(os.environ)
    # This is a Linux host executable, so do not inherit guest loader shims.
    environment.pop("LD_PRELOAD", None)
    if vendor:
        environment["__EGL_VENDOR_LIBRARY_FILENAMES"] = str(vendor)
    arguments = [str(program), "--frames", str(frames), "--cycles", str(cycles), "--samples", str(samples)]
    if device: arguments += ["--device", str(device)]
    def constrain_child():
        if affinity is not None: os.sched_setaffinity(0, affinity)
        if memory_limit is not None:
            _, hard = resource.getrlimit(resource.RLIMIT_AS)
            limit = min(memory_limit, hard) if hard != resource.RLIM_INFINITY else memory_limit
            resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=70, env=environment,
                                preexec_fn=constrain_child if affinity is not None or memory_limit is not None else None)
        data = json.loads(result.stdout)
        if not isinstance(data, dict): raise ValueError("Unexpected probe result")
        data["exit_status"] = result.returncode
        data["ok"] = bool(data.get("ok") and result.returncode == 0)
        renderer = str(data.get("renderer", "")).lower()
        data["software_renderer"] = any(name in renderer for name in
            ("llvmpipe", "lavapipe", "softpipe", "swiftshader", "software rasterizer"))
        data["hardware_usable"] = data["ok"] and not data["software_renderer"]
        nodes = (data.get("drm_render_node"), data.get("drm_device_node"))
        for node in nodes:
            if not node: continue
            pci = read(Path("/sys/class/drm") / Path(node).resolve().name / "device/uevent")
            address = next((line[14:] for line in pci.splitlines() if line.startswith("PCI_SLOT_NAME=")), None)
            if address: data["pci"] = address; break
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
    parser.add_argument("--gpu", help="Verify rendering on the GPU at this PCI address")
    parser.add_argument("--frames", type=int, default=120, help="Offscreen draw/readback timing samples")
    parser.add_argument("--cycles", type=int, default=16, help="Context/surface lifecycle repetitions")
    parser.add_argument("--samples", type=int, default=0, help="Requested MSAA sample count")
    args = parser.parse_args()
    if args.gpu and not args.graphics: parser.error("--gpu requires --graphics")
    if not 1 <= args.frames <= 10000 or not 0 <= args.cycles <= 1000 or not 0 <= args.samples <= 64:
        parser.error("Probe counts are outside their supported bounds")
    source = Path(__file__).resolve().parents[2]
    prefix = Path(__file__).resolve().parents[1]
    if Path(__file__).resolve().parent.parent.name == "libexec":
        prefix = Path(__file__).resolve().parents[2]
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
    cgroup = cgroup_inventory()
    cpu = cpu_inventory()
    cpu["effective_parallelism"] = min(cpu["allowed_cpus"] or 1, max(1, math.ceil(cgroup["cpu_capacity"]))) if cgroup["cpu_capacity"] is not None else cpu["allowed_cpus"]
    report = {"schema_version": 2, "system": {"kernel": platform.release(),
        "distribution": read("/etc/os-release"), "flatpak": Path("/.flatpak-info").exists()},
        "cpu": cpu, "memory": memory_inventory(cgroup), "cgroup": cgroup, "gpus": gpu_inventory(),
        "desktop": {"x11_environment": bool(os.environ.get("DISPLAY")),
                    "wayland_environment": bool(os.environ.get("WAYLAND_DISPLAY"))},
        "audio": {"pipewire_socket_present": (runtime_dir / "pipewire-0").exists(),
                  "pulse_socket_present": (runtime_dir / "pulse/native").exists()},
        "runtime": {"kind": metadata_kind, "data": runtime} if runtime else None, "graphics": None}
    if args.graphics:
        probes = [args.probe] if args.probe else [source / "build/tools/cider/cider-gl-probe",
                  prefix / "libexec/ciderdarling/cider-gl-probe"]
        executable = next((path for path in probes if path and path.is_file()), None)
        device = None
        if args.gpu:
            adapter = next((gpu for gpu in report["gpus"] if gpu["pci"] == args.gpu), None)
            if not adapter: parser.error("Requested PCI GPU is absent from the DRM inventory")
            nodes = adapter["render_nodes"] or adapter["card_nodes"]
            if not nodes: parser.error("Requested GPU exposes no DRM device node")
            device = nodes[0]
        report["graphics"] = graphics_probe(executable, args.egl_vendor, device=device, frames=args.frames,
                                            cycles=args.cycles, samples=args.samples) if executable else {
            "ok": False, "error": "Build cider-gl-probe or pass --probe PATH"}
        if args.gpu:
            graphics = report["graphics"]
            graphics["requested_pci"] = args.gpu
            graphics["selection_verified"] = graphics.get("pci") == args.gpu and graphics.get("device_matches_request") is True
            if not graphics["selection_verified"]: graphics["ok"] = graphics["hardware_usable"] = False
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
