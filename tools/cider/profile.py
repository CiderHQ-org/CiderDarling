#!/usr/bin/env python3
"""Summarize numeric Cider traces and optionally sample a chosen Linux process."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics
import time


def percentiles(values):
    values = sorted(values)
    if not values: return None
    return {"count": len(values), "p50_ns": int(statistics.median(values)),
            "p95_ns": values[(len(values) * 95 + 99) // 100 - 1],
            "p99_ns": values[(len(values) * 99 + 99) // 100 - 1], "maximum_ns": values[-1]}


def summarize(path):
    timings, presents = defaultdict(list), defaultdict(list)
    rejected = 0
    with Path(path).open() as source:
        consumed = 0
        for line in source:
            consumed += len(line)
            if consumed > 64 * 1024 * 1024: raise ValueError("Trace exceeds the 64 MiB analysis limit")
            try:
                record = json.loads(line)
                category = record["category"]
                if category not in {"cgl_bind", "cgl_bind_error", "cgl_present", "cgl_present_error",
                                    "workqueue_wait", "workqueue_wait_timeout", "condition_wait"}: raise ValueError()
                numeric = [record[key] for key in ("pid", "thread", "timestamp_ns", "duration_ns", "value")]
                if not all(isinstance(value, int) and value >= 0 for value in numeric): raise ValueError()
                timings[category].append(record["duration_ns"])
                if category == "cgl_present": presents[(record["pid"], record["value"])].append(record["timestamp_ns"])
            except (ValueError, KeyError, TypeError): rejected += 1
    intervals, gaps = [], []
    for (pid, context), values in presents.items():
        values.sort()
        intervals.extend(second - first for first, second in zip(values, values[1:]))
        gaps.extend({'pid': pid, 'context': context, 'timestamp_ns': second,
                     'interval_ns': second - first}
                    for first, second in zip(values, values[1:]) if second - first >= 100000000)
    return {"schema_version": 1, "categories": {key: percentiles(value) for key, value in timings.items()},
            "presentation_intervals": percentiles(intervals), "rejected_records": rejected,
            "long_presentation_gaps": {'threshold_ns': 100000000, 'count': len(gaps),
                                       'largest': sorted(gaps, key=lambda item: item['interval_ns'], reverse=True)[:20]},
            "measurement": "API call durations and per-context presentation intervals; not GPU execution time"}


def process_sample(pid):
    root = Path("/proc") / str(pid)
    status = {}
    for line in (root / "status").read_text().splitlines():
        key, _, value = line.partition(":")
        if key in ("Threads", "VmRSS", "VmSize", "VmSwap", "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches"):
            status[key] = value.strip()
    status["timestamp_ns"] = time.monotonic_ns()
    status["scheduler"] = []
    for thread in sorted((root / "task").iterdir()):
        try:
            fields = (thread / "schedstat").read_text().split()
            status["scheduler"].append({"tid": int(thread.name), "running_ns": int(fields[0]), "runnable_wait_ns": int(fields[1])})
        except (OSError, ValueError, IndexError): pass
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", nargs="?", type=Path)
    parser.add_argument("--pid", type=int, help="Sample only the process with this Linux PID")
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.trace and not args.pid: parser.error("Supply a trace file or --pid")
    if not 0 < args.seconds <= 300 or args.pid is not None and args.pid <= 0: parser.error("Invalid sampling bounds")
    result = summarize(args.trace) if args.trace else {"schema_version": 1}
    if args.pid:
        samples = []
        finish = time.monotonic() + args.seconds
        while time.monotonic() < finish:
            try: samples.append(process_sample(args.pid))
            except OSError: break
            time.sleep(0.1)
        result["process_samples"] = samples
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        with args.output.open("x") as destination: destination.write(text)
    else: print(text, end="")


if __name__ == "__main__":
    main()
