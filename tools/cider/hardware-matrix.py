#!/usr/bin/env python3
"""Record repeatable host rendering scenarios without launching a client."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import doctor


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="New JSON report; existing files are preserved")
    parser.add_argument("--egl-vendor", type=Path)
    parser.add_argument("--gpu", help="PCI address, or all detected GPUs when omitted")
    parser.add_argument("--frames", type=int, default=240)
    parser.add_argument("--memory-limit-mib", type=int, help="Optional child virtual-address limit; driver reservations also count")
    args = parser.parse_args()
    if args.output.exists(): parser.error("Output file already exists")
    if not 1 <= args.frames <= 10000: parser.error("--frames must be in 1..10000")
    if args.memory_limit_mib is not None and args.memory_limit_mib < 256: parser.error("Memory limit must be at least 256 MiB")
    adapters = doctor.gpu_inventory()
    if args.gpu: adapters = [gpu for gpu in adapters if gpu["pci"] == args.gpu]
    if not adapters: parser.error("No requested DRM adapter was found")
    available = sorted(os.sched_getaffinity(0))
    scenarios = [("baseline", 0, 32, None), ("msaa4", 4, 32, None),
                 ("one_cpu", 0, 32, set(available[:1])), ("two_cpus", 0, 32, set(available[:2])),
                 ("lifecycle100", 0, 100, None)]
    results = []
    for gpu in adapters:
        nodes = gpu["render_nodes"] or gpu["card_nodes"]
        for name, samples, cycles, affinity in scenarios:
            print(f"Checking {gpu['pci']}: {name}", flush=True)
            graphics = doctor.graphics_probe(args.probe.resolve(), args.egl_vendor, device=nodes[0],
                frames=args.frames, cycles=cycles, samples=samples, affinity=affinity,
                memory_limit=args.memory_limit_mib * 1024 * 1024 if args.memory_limit_mib else None)
            results.append({"pci": gpu["pci"], "scenario": name, "affinity": sorted(affinity) if affinity else available,
                            "result": graphics})
    report = {"schema_version": 1, "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "kernel": platform.release(), "cpu": doctor.cpu_inventory(), "cgroup": doctor.cgroup_inventory(),
              "memory": doctor.memory_inventory(), "gpus": adapters, "results": results,
              "coverage": {"host_offscreen": True, "gameplay": False, "onscreen_presentation": False,
                           "input_audio": "Requires real-device integration fixtures and manual checks"}}
    with args.output.open("x") as destination: json.dump(report, destination, indent=2); destination.write("\n")
    return 0 if all(item["result"].get("hardware_usable") and item["result"].get("device_matches_request") is True for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
