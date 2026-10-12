#!/usr/bin/env python3
"""Additional CPU, workqueue and socket regressions against prepared sources."""
import importlib.util
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("source_harness", ROOT / "validation/run.py")
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


def main():
    compiler = os.environ.get("CC", "clang")
    xnu = harness.XNU
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    with tempfile.TemporaryDirectory(prefix="cider-runtime-source-") as name:
        temporary = Path(name)
        common = [compiler, "-O2", "-g", "-Wall", "-Wextra", "-Wno-unused-function", "-pthread",
                  "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", temporary]
        cpu = (xnu / "src/xnu_syscall/bsd/helper/misc/sysctl_machdep.c").read_text()
        harness.write(temporary, "cpu-source.h", cpu[cpu.index("#ifndef CIDER_CPUID"):])
        affinity = (xnu / "src/xnu_syscall/bsd/helper/misc/sysctl_hw.c").read_text()
        begin = affinity.index("static int cider_affinity")
        harness.write(temporary, "affinity-source.h", affinity[begin:affinity.index("static sysctl_handler(handle_availcpu)", begin)])
        harness.run([*common, ROOT / "validation/cpu.c", "-o", temporary / "cpu"])
        harness.run([temporary / "cpu"], env=env)
        workq = (xnu / "src/xnu_syscall/bsd/impl/bsdthread/workq_kernreturn.c").read_text()
        begin = workq.index("static struct wq_kevent_data* workq_events")
        code = workq[begin:workq.index("static void rpc_guard", begin)]
        begin = workq.index("static int64_t workq_now_ns")
        code += workq[begin:workq.index("#define __PTHREAD_PRIORITY_CBIT_USER_INTERACTIVE", begin)]
        harness.write(temporary, "semaphore-source.h", code)
        public = (xnu / "include/xnu_syscall/bsd/impl/bsdthread/workq_kernreturn.h").read_text()
        begin = public.index("struct wq_kevent_data\n")
        harness.write(temporary, "event-structure.h", public[begin:public.index("};", begin) + 2])
        harness.run([*common, ROOT / "validation/semaphore.c", "-o", temporary / "semaphore"])
        harness.run([temporary / "semaphore"], env=env)
        psynch = xnu / "src/xnu_syscall/bsd/impl/psynch"
        code, cases = "", []
        for index, path in enumerate(sorted(psynch.glob("psynch_*.c"))):
            source = path.read_text()
            if "RPC reply codes are positive" not in source: continue
            name = path.stem
            code += f"#define dserver_rpc_{name}(...) fake_rpc(&retval)\n"
            code += harness.function(source, f"long sys_{name}(")
            signature = code[code.rindex(f"long sys_{name}("):].split("{", 1)[0]
            count = len(signature.split("(", 1)[1].rsplit(")", 1)[0].split(","))
            cases.append(f"case {len(cases)}: return sys_{name}({','.join(['0'] * count)});")
        code += "static long call_psynch(int selected) { switch(selected) {" + "".join(cases) + "default: abort();} }\n"
        harness.write(temporary, "psynch-source.h", code)
        harness.run([*common, "-Wno-unused-parameter", ROOT / "validation/psynch.c", "-o", temporary / "psynch"])
        harness.run([temporary / "psynch"], env=env)
        includes = temporary / "include/darling/emulation"
        includes.mkdir(parents=True)
        for path in (xnu / "include").iterdir():
            (includes / path.name).symlink_to(path)
        overrides = temporary / "overrides"
        harness.write(overrides, "darling/emulation/common/base.h", """
#include <stdint.h>
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
#define LINUX_SYSCALL(...) ({ int saved = errno; long result = syscall(__VA_ARGS__); if (result == -1) result = -(long)errno; errno = saved; result; })
""")
        harness.write(overrides, "darling/emulation/xnu_syscall/bsd/helper/bsdthread/cancelable.h", "#define CANCELATION_POINT()\n")
        flags = (xnu / "src/conversion/network/duct.c").read_text()
        conversions = '#include <darling/emulation/conversion/network/duct.h>\n'
        for signature in ("int msgflags_bsd_to_linux(", "int msgflags_linux_to_bsd("):
            conversions += harness.function(flags, signature)
        harness.write(temporary, "message-flags.c", conversions)
        network = xnu / "src/xnu_syscall/bsd/impl/network"
        harness.run([*common, "-I", overrides, "-I", temporary / "include",
                     ROOT / "validation/messages.c", network / "sendmsg.c", network / "recvmsg.c",
                     xnu / "src/conversion/network/recvmsg.c", temporary / "message-flags.c",
                     "-o", temporary / "messages"])
        harness.run([temporary / "messages"], env=env)


if __name__ == "__main__":
    main()
