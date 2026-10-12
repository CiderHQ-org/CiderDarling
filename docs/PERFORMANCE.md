# Performance and hardware validation

CiderDarling targets x86_64 Linux and is hosted independently at
https://github.com/CiderHQ-org/CiderDarling. This batch extends the foundation
to 59 dependency file patches, with normal root-repository changes alongside
them. Upstream pins and licenses are retained.

## Implemented areas

| Area | Behavior | Evidence |
| --- | --- | --- |
| CPU reporting | Correct CPUID leaves, family/model/brand decoding, integer sysctl values, buffer bounds and OS gating of AVX/FMA/F16C | Intel/AMD register fixtures, real affinity changes, native sysctl queries |
| Parallelism | Supply libdispatch's missing `hw.activecpu`; respect affinity and ancestor CPU quotas; scale worker capacity with available CPUs | Native fixture reached 16 simultaneous workers on 16 available logical CPUs |
| Workqueues and synchronization | Atomic token semaphores, monotonic deadlines, signal retry, worker-owned event completion and bounded returned event counts | 128,000 contended operations; owner/bounds checks; native parallel execution |
| Condition variables | Return RPC errno as negative syscall errors instead of sequence updates in nine psynch wrappers | Fault injection, unsigned sequence checks, native condition timeout/wakeup |
| Framebuffers | Honor color, alpha, depth, stencil and MSAA; report selected values and failed operations | Source fixtures, host driver checks, native CGL rendering |
| Native windows | Match X11 parent and GL/layer child visuals to EGL formats; release their colormaps; preserve context swap intervals | Real NVIDIA window creation, resize, presentation and destruction |
| Graphics lifetime | Retain thread bindings until switch/exit; provide an offscreen drawable; detach destroyed surfaces and report swap failures | 1,000 source lifecycle cycles, native offscreen/window fixtures |
| Socket messages | Validate complete control-message chains, convert SCM_RIGHTS layouts, bound truncation and close undeliverable descriptors | Real sockets, malformed buffers, repeated truncated receives and descriptor counts |
| Hardware and pressure | Verify actual DRM/PCI renderer identity; inspect ancestor memory/CPU/PID limits and headroom; record lifecycle RSS; bound workers/traces | Strict RTX 3060 Ti identity, MSAA/sharing, parent-limit tests, restricted-CPU scenarios |
| Diagnostics and deployment | Numeric timing traces, scheduler/RSS sampling, repeatable hardware reports, private runtime staging and version selection | Clang/GCC fixtures, full stock build, actual private-prefix Mach-O execution |

The generated `gen/bsdsyscalls/SYS.h` is patched along with its canonical source.
The build actually uses the generated header: changing only the canonical one
does not repair exported ulock functions. Validation checks equality of the two
and audits each dependency edit against its pinned original and recorded patch.

## Threading and resource policy

Applications and libdispatch can use every available logical CPU. The workqueue
resource ceiling is `max(64, 4 * effective_cpu_capacity)`, capped at 16,384 workers.
This leaves room for blocked workers while CPU capacity remains governed by
affinity and cgroup quotas. It does not force that many CPU-bound workers to run.
Saturation returns EAGAIN instead of blocking the requesting worker indefinitely.
Idle parked workers retain the existing 15-second retirement policy, with fixed
timeout/token handling. The application controls how much work it submits.

Thread stacks reserve address space rather than eagerly populating every page.
Caller-owned stacks retain their original policy. Memory diagnostics use ancestor
headroom, including memory already used by other processes in the parent group.
External driver and proprietary client caches are not forcibly capped.

SCM_RIGHTS is the supported ancillary payload in this batch. Unsupported send
types return ENOTSUP; unsupported receive types are omitted with CTRUNC. Full
packet-info, timestamp and credential conversion remain separate work, along with
priority inheritance, shared ulocks and complete workloop semantics.

Swap interval settings belong to their context and are applied when it binds a
window drawable. AppKit's internal X11 painting context uses interval 0 so an
occluded window does not hold its UI thread waiting for vblank; application
contexts keep their own requested policy. Negative adaptive intervals are not
implemented. Tracing the small background-window fixture exposed approximately
one-second swap waits before this repair; after it, the measured presentation
p95 was about 14 ms on this machine. This is a fixture result, not gameplay FPS.

## Private runtime execution and rollback

```sh
python3 tools/cider/isolate.py --stage --run
python3 tools/cider/isolate.py --stage --run --egl-vendor /usr/share/glvnd/egl_vendor.d/10_nvidia.json
```

Staging configures a matching compiled installation path inside this checkout,
builds with a CPU/memory-aware job count, and installs there without stopping
system Darling. The automatic build count was 16 on the development machine.
`--runtime PATH` selects another staged version. Keeping previous directories
provides rollback without replacing system files or migrating a live prefix.

Execution requires Linux unprivileged user namespaces. A new user/mount/PID
namespace maps only the invoking user and mounts its own procfs. Each run uses
a fresh short prefix under `build/` to keep AF_UNIX socket paths within their
limit. The runner shuts down only that prefix; its private PID namespace removes
remaining test processes on exit. Logs, traces and `results.json` are retained
under `build/cider-isolation/runs/`. Existing Cider prefixes and authentication
data are not used. Graphics tests briefly create small background windows and
close them without activating or modifying existing application windows.

The fixtures compile against the selected runtime libraries and this checkout's
SDK. Staging checks the patch manifest fingerprint before executing fixtures, so
changes during a build require staging again. `CIDER_SHUTDOWN_ON_INSTALL` defaults
to OFF; enabling it restores upstream's system-wide shutdown hook. The hook is
skipped for DESTDIR packaging. Private staging always keeps it OFF.

The current native fixture passes exported ulock/wait2 errno checks, CPU queries,
pthread stack attributes, condition-variable timeout/wakeup, 16-worker parallel
execution, datagram flags and kqueue close cleanup. Graphics checks pass event
delivery in default/tracking/modal modes, 32 offscreen context cycles, and four
native window lifetimes with five resizes/presentations each. The renderer reports
NVIDIA OpenGL 4.1 on the RTX 3060 Ti. This does not certify Roblox gameplay or
absent AMD/Intel hardware.

## Collecting timing and hardware reports

```sh
python3 tools/cider/doctor.py --json --graphics --gpu PCI_ADDRESS --samples 4 --cycles 100
python3 tools/cider/hardware-matrix.py --probe build/tools/cider/cider-gl-probe --output build/hardware-report.json
python3 tools/cider/profile.py build/cider-isolation/runs/RUN/events.jsonl
python3 tools/cider/profile.py --pid LINUX_PID --seconds 10 --output build/process-report.json
```

Add `--egl-vendor` to select an installed vendor manifest for a child probe.
Output reports preserve existing files. The matrix runs baseline, MSAA, one-CPU,
two-CPU and 100-lifecycle scenarios for each detected adapter. Its optional
`--memory-limit-mib` restricts child virtual address space; driver reservations
also count, so it is not equivalent to testing a low-RAM machine. All five
scenarios passed on the development RTX 3060 Ti with verified device identity.

`CIDER_TRACE_FILE` enables numeric traces for CGL binding/presentation, workqueue
waits and condition-variable RPCs. Use a dedicated guest-visible file; the private
runner creates one automatically. Files must be user-owned, regular, not symlinks
and inaccessible to group/other users. Production has per-component record limits
and an approximately 32 MiB file limit; analysis is bounded at 64 MiB. No app text,
URLs, credentials or shader contents are recorded. Tracing is disabled by default.
The profiler reports API-duration tails and per-context presentation intervals,
which are not GPU execution times or full gameplay frame times. Scheduler sampling
reads numeric `/proc` data only for the explicitly selected process.

Installed helpers are `ciderdarling-doctor`, `ciderdarling-profile` and
`ciderdarling-hardware-matrix`. Real-device checks still need gameplay start/join/
leave/shutdown, fullscreen/minimize, mixed/fractional DPI, monitor/audio hotplug,
keyboard layouts/IME, controllers, suspend/resume and device loss. Test NVIDIA
generations, AMD GCN/RDNA/APUs, Intel integrated/Arc and hybrid laptops, including
low-core machines and background load. One GPU and host probes do not complete
those checks. Native Wayland, Metal/Vulkan and broader distro packaging remain
separate integration workstreams.
