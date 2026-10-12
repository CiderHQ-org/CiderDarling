# CiderDarling runtime foundation

Target repository: https://github.com/CiderHQ-org/CiderDarling

This project targets x86_64 Linux hosts running the x86_64 macOS Roblox client
through Cider. ARM support is outside this workstream. Hardware support labels
require results from real machines; successfully compiling a backend does not
establish gameplay compatibility.

## Source changes

Dependency fixes are stored as ordinary unified diffs in `patches/cider/`.
`series.json` records the complete original, replacement, and patch SHA-256 for
each file. CMake invokes `tools/cider/prepare.py` before compiling dependencies.
The script validates the complete series before edits, recognizes previously
applied files, rejects unknown/local source changes, and replaces each file
atomically. An interrupted write can be resumed by rerunning preparation.

The parent repository intentionally retains the upstream submodule commit pins.
Applied dependencies therefore appear modified in `git status`; their changes
are reproducible from the committed series. Do not commit a submodule pointer
to an unpublished dependency commit. Upstream dependency URLs are absolute so
cloning this fork under another organization preserves dependency resolution.

| Area | Implemented changes | Verification scope |
| --- | --- | --- |
| Kqueue map | Bound walks by inserted descriptors; include the final valid descriptor; fix CAS removal and redundant post-delete stores | Actual source compiled with ASan/UBSan; boundary, callback, deletion/reuse checks |
| Listening sockets | Correct allocation sizes and swap-removal indexing | Concurrent registration/removal stress under ASan/UBSan |
| Datagram receives | Pass translated Darwin flags to Linux | Real local datagrams, nonblocking empty receives, PEEK |
| Ulock | Correct exported x86_64 NO_ERRNO behavior; implement relative nanosecond wait2 for existing 32-bit compare/unfair-lock opcodes | Actual wait/wake sources and exported-wrapper macro, real futex wake, timeout/error/errno checks |
| Connectx | Basic source bind, destination connect, optional initial send; preserve EINPROGRESS and partial byte counts | Actual source with mocked lower syscall boundary; unsupported options rejected |
| Workqueues | Reset the worker stack and frame pointer on every x86_64 reuse; propagate creation failures and release the worker slot | 10,000 assembly reuses at O0/O2 with argument/alignment checks; compilation |
| Pthread/loader | 8 MiB default/loader-owned stacks; preserve caller stacks; detect allocation/guard/creation errors; atomically publish thread startup | Actual allocation/creation functions with failure injection and ownership checks |
| OpenGL | Preserve format terminators/valued attributes; select requested core profiles; bind desktop GL per thread; report failed switches without publishing a false current context | Actual CGL functions under ASan/UBSan and mock EGL driver failures/profile negotiation |
| QuartzCore | Restore the previous context after layer rendering, including exceptions | Full framework compilation; runtime UI fixture remains to be run |
| AppKit | Synchronize event queue access; signal a run-loop source for background posts; preserve unmatched events; fix discard matching and periodic-event removal | Full framework compilation; real Cocoa wakeup/mode fixture remains to be run |
| Displays | Decode EDID centimetres into millimetres; provide nonzero fallback dimensions | Compilation; cross-monitor validation remains |
| Strings | Compile portable string primitives with O2 and no builtin recursion | Actual sources checked for overlap, alignment, tails, comparison behavior and guarded-page reads |
| Toolchains | Query supported constant-literal flags; replace incompatible empty literals; retain Objective-C debug-mode consistency; guard LLVM branch opcodes; avoid unsupported relative switch-table binds in libunwind | Full Clang 23 stock build with Metal off; Metal/other LLVM versions require builds |
| Metal | Retain the supplied command queue correctly | Source repair; experimental backend remains opt-in |
| libsystem_m | Compile `sincos.c` with `-fno-builtin` so Clang does not turn `__sincosf_stret`/`__sincos_stret` into calls to themselves (unbounded recursion; Roblox died with SIGSEGV at startup) | Disassembly of the rebuilt library; Roblox started under Zink and OpenGL, with and without MangoHud |

`__ulock_wait2` does not implement shared/64-bit comparisons, absolute deadlines,
or Darwin priority inheritance. Targeted ulock wakes retain the existing
upstream limitations. `connectx` rejects source-interface selection,
nondefault associations, deferred-connect/TFO flags, and malformed iovecs.
These APIs must not be described as complete implementations.

The thread stack policy reserves virtual address space; it does not eagerly
populate every page. `CIDER_THREAD_STACK_KB` controls the default from 512 to
65536 KiB, with 8192 as the default. Caller-provided stacks remain owned and
sized by their caller.

## Build and rollback

Install the upstream development dependencies for your distribution, then:

```sh
python3 tools/cider/bootstrap.py --jobs 4
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_METAL=OFF -DTARGET_i386=OFF
cmake --build build --parallel 4
python3 validation/run.py
```

The bootstrap disables authentication prompts for public dependencies and
leaves optional Swift SDK LFS objects as pointer files. It changes LFS filter
configuration only inside this checkout's Swift submodule. Swift SDK support
is not part of this build configuration.

Review/reverse the dependency series without resetting other work:

```sh
python3 tools/cider/prepare.py --check
python3 tools/cider/prepare.py --reverse --check
python3 tools/cider/prepare.py --reverse
```

Root-repository changes remain normal Git changes. To compile a reversed
dependency baseline, use a separate build directory with
`-DCIDER_APPLY_PATCHES=OFF`; normal configuration reapplies the series.

Build, validation, bootstrap, and doctor commands do not install over system
Darling or open Cider authentication data. `CIDER_SHUTDOWN_ON_INSTALL` defaults
to OFF; enabling it preserves upstream's system-wide shutdown hook, skipped for
DESTDIR packaging. Private staging keeps this option OFF. Live installation
remains a separate step.

## Host diagnostics

```sh
python3 tools/cider/doctor.py --json
python3 tools/cider/doctor.py --json --graphics
python3 tools/cider/doctor.py --json --graphics --egl-vendor /usr/share/glvnd/egl_vendor.d/10_nvidia.json
```

The host probe draws its own small shader triangle, checks actual pixel
readback, and verifies a shared texture from a second thread. It uses an
offscreen pbuffer, with a device-display fallback for headless terminals.
It does not test on-screen presentation or Roblox shaders. Each vendor probe
runs in a child, preserving host settings and GPU selection outside that child.
Supply a manifest appropriate to the installed driver rather than assuming
the example NVIDIA path exists on every distribution.

The report contains CPU features/affinity, DRM adapter identities, exposed
memory limits, desktop/audio socket availability, runtime metadata, and
optional rendering results. Authentication data and game logs are not read.
Exit status 0 means the requested checks passed, 1 means a graphics check
failed, and 2 identifies a software renderer. Inventory-only checks return 0.
Source-build metadata is explicitly distinguished from a selected installed
sysroot. Probe success does not establish that system Darling uses that build.

The build installs a `ciderdarling-doctor` helper and records source fixes in
`usr/share/ciderdarling/runtime.json` within the runtime root. Cider should
consult metadata from the runtime it actually launches, and retire individual
shim workarounds only after their corresponding integration fixtures pass.
Current Cider settings and fallback shims have not been modified by this work.

`validation/darwin-runtime.c` is a standalone Mach-O integration fixture for
the runtime actually installed in a dedicated prefix. It exercises exported
ulock/wait2 symbols without the Cider shim, pthread default attributes,
nonblocking/PEEK datagrams, and kqueue close cleanup. Its thirty-second alarm
bounds missing/incorrect waits. The 8 MiB assertion assumes the default build
policy. The fixture now also checks CPU sysctl types, active CPU capacity,
condition-variable timeout/wakeup and libdispatch parallel work. It has been
executed successfully against the private staged runtime. See
[the current validation record](PERFORMANCE.md).

For a development machine with Darling's libraries in `/usr/libexec/darling`:

```sh
mkdir -p build/validation
clang -target x86_64-apple-darwin20 -mmacosx-version-min=11.0 \
  -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 \
  -isysroot "$PWD/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" \
  -fuse-ld=lld -L/usr/libexec/darling/usr/lib -O2 \
  validation/darwin-runtime.c -o build/validation/darwin-runtime
```

Adjust the library root for a separate installed runtime. Running the fixture
requires a dedicated test prefix and the matching new runtime; the compiled
file is not executed automatically by the host harness.

## Validation record

The initial implementation was built locally on a Ryzen 7 5800X with Clang
23.1.1, CMake 4.4.4, Ninja, and the stock x86_64 component set with Metal off.
Source fixtures were also exercised with GCC. ASan/UBSan are used where
appropriate; leak scanning is disabled in the fixture harness because it
cannot enumerate threads in the restricted development container.

On the RTX 3060 Ti / NVIDIA 615.71.09, explicit NVIDIA EGL through an offscreen
device display passed draw/readback and cross-thread sharing at OpenGL 4.1.
Default vendor selection in the development environment produced llvmpipe;
the doctor correctly identifies that as software rendering. These are host
probe results, not Cider gameplay or broad hardware certification.

GitHub CI checks source preparation and fixtures with Clang and GCC on Ubuntu.
The existing complete Debian/source build workflow prepares the same series.
CI results require the repository to be published and workflows to run.

## Next work

1. Keep the passing isolated-prefix Mach-O, AppKit and CGL regressions running
   on supported builds. Validate startup, join, leave, and shutdown with the
   actual client and corresponding migrated shim fixes disabled.
2. Investigate the remaining long leave-game frame gap separately from kqueue
   cleanup, plus condition-variable sequence errors and lost wakeups. Preserve
   cancellation, process-shared behavior, and ABI semantics in future fast paths.
3. Exercise NVIDIA generations, AMD GCN/RDNA/APUs, Intel integrated/Arc, and
   hybrid laptops across X11/Xwayland, stock drivers, and Zink. Include older
   Intel/AMD CPUs, low-core systems, background load, and power limits.
4. Validate mixed/fractional DPI, negative monitor coordinates, hotplug,
   refresh changes, fullscreen/minimize, suspend/resume, and device loss.
5. Repair CoreAudio format/device negotiation and cookie APIs; add USB/HDMI/
   Bluetooth devices, keyboard layouts/IME, and controller discovery/hotplug.
6. Develop supported rootless operation and staged packaging. Then broaden
   distro builds and add clearly scoped driver workarounds/capability caching.
7. Complete native Wayland presentation and Metal/Vulkan shader/encoder,
   resource lifetime, depth/stencil/MSAA and swapchain work as separate
   experiments, qualified by actual scenes and lifecycle tests.

Dependency patches retain the licenses of their source projects. No Roblox
executables, proprietary shader packs, authentication material, or copied
driver binaries are committed.
