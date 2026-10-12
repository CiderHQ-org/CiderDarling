#!/usr/bin/env python3
"""Build and run regressions using the actual prepared dependency sources."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
XNU = ROOT / "src/external/xnu/darling/src/libsystem_kernel/emulation"


def run(args, **kwargs):
    print("+ " + " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, timeout=120, **kwargs)


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    # Used only for reviewed C functions without braces in string literals.
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


def write(root, relative, content):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    return path


def main():
    run(["python3", ROOT / "tools/cider/prepare.py", "--check"])
    run(["python3", ROOT / "tools/cider/audit-series.py"])
    run(["python3", ROOT / "validation/prepare_test.py"])
    run(["python3", ROOT / "validation/diagnostics_test.py"])
    run(["python3", ROOT / "validation/runtime-source.py"])
    compiler = os.environ.get("CC", "clang")
    environment = dict(os.environ)
    # LeakSanitizer cannot enumerate threads in the restricted Codex container.
    # Address/undefined behavior checks remain enabled.
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
    with tempfile.TemporaryDirectory(prefix="cider-validation-") as name:
        temporary = Path(name)
        common = [compiler, "-O2", "-g", "-Wall", "-Wextra", "-pthread",
                  "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        run([*common, ROOT / "validation/kqueue.c", "-o", temporary / "kqueue"])
        run([temporary / "kqueue"], env=environment)

        includes = temporary / "include"
        (includes / "darling/emulation").mkdir(parents=True)
        # Real public emulation headers plus narrow Linux-host ABI adapters.
        for path in (XNU / "include").iterdir():
            (includes / "darling/emulation" / path.name).symlink_to(path)
        overrides = temporary / "overrides"
        def header(path, content):
            write(overrides, "darling/emulation/" + path, content)
        header("common/base.h", "#include <stdint.h>\n")
        header("conversion/duct_errno.h", "")
        header("linux_premigration/linux-syscalls/linux.h", """
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#define LINUX_EAGAIN EAGAIN
#define LINUX_SYSCALL(...) ({int saved = errno; long result = syscall(__VA_ARGS__); if (result == -1) result = -(long)errno; errno = saved; result;})
""")
        header("xnu_syscall/bsd/helper/bsdthread/cancelable.h", """
extern long sys_pthread_canceled(int);
#define CANCELATION_POINT() if (sys_pthread_canceled(0) == 0) return -4
""")
        # Expand the actual x86_64 exported-wrapper macro for a Linux assembly
        # harness; verify cerror semantics as well as the raw futex functions.
        sys_header = (ROOT / "src/external/xnu/gen/bsdsyscalls/SYS.h").read_text()
        canonical = (ROOT / "src/external/xnu/libsyscall/custom/SYS.h").read_text()
        assert sys_header == canonical, "Compiled syscall header differs from its canonical source"
        tail = sys_header[sys_header.index("#elif defined(__x86_64__)"):]
        begin = tail.index("\t#define UNIX_SYSCALL_NONAME")
        macro = tail[begin:tail.index("#else", begin)]
        wrappers = """
#define SYS_ulock_wait 515
#define SYS_ulock_wake 516
#define SYS_ulock_wait2 544
#define SYS_getpid 20
#define BRANCH_EXTERN(symbol) jmp symbol
""" + macro
        for api, count in (("ulock_wait", 4), ("ulock_wait2", 5), ("ulock_wake", 3), ("getpid", 0)):
            suffix = {"ulock_wait": "wait", "ulock_wait2": "wait2", "ulock_wake": "wake", "getpid": "getpid"}[api]
            wrappers += f"\n.text\n.globl _test_{suffix}\n_test_{suffix}:\nUNIX_SYSCALL_NONAME({api}, {count}, cerror_nocancel)\nret\n"
        wrappers += """
.globl __darling_bsd_syscall
__darling_bsd_syscall:
    cmpl $515, %eax
    je .wait
    cmpl $544, %eax
    je .wait2
    cmpl $516, %eax
    je .wake
    call fake_getpid
    ret
.wait:
    call sys_ulock_wait
    ret
.wait2:
    call sys_ulock_wait2
    ret
.wake:
    call sys_ulock_wake
    ret
.section .note.GNU-stack,"",@progbits
"""
        assembly = write(temporary, "wrappers.S", wrappers)
        psynch = XNU / "src/xnu_syscall/bsd/impl/psynch"
        run([compiler, "-O2", "-g", "-pthread", "-I", overrides, "-I", includes,
             ROOT / "validation/ulock.c", psynch / "ulock_wait.c", psynch / "ulock_wake.c",
             assembly, "-o", temporary / "ulock"])
        run([temporary / "ulock"])

        flags_text = (XNU / "src/conversion/network/duct.c").read_text()
        flags = write(temporary, "network-flags.c",
                      '#include <darling/emulation/conversion/network/duct.h>\n' +
                      function(flags_text, "int msgflags_bsd_to_linux("))
        network = XNU / "src/xnu_syscall/bsd/impl/network"
        run([compiler, "-O2", "-g", "-I", overrides, "-I", includes,
             ROOT / "validation/network.c", network / "recvfrom.c", network / "connectx.c",
             flags, "-o", temporary / "network"])
        run([temporary / "network"])

        gl = (ROOT / "src/frameworks/OpenGL/OpenGL.c").read_text()
        gl_code = 'static EGLDisplay display;\nstatic EGLConfig config;\nstatic pthread_mutex_t g_displaysMutex = PTHREAD_MUTEX_INITIALIZER;\nstatic CGLContextObj g_contexts;\n'
        for tag in ("struct _CGLContextObj", "struct _CGLPixelFormatObj", "struct _CGLWindow"):
            begin = gl.index(tag + " {")
            gl_code += gl[begin:gl.index("};", begin) + 2] + "\n"
        gl_code += 'static struct _CGLWindow* g_windows;\n'
        for signature in ("static inline int attribute_has_argument(", "static int attributes_count(",
                          "static CGLError choose_config(", "CGLError CGLChoosePixelFormat(", "CGLError CGLDescribePixelFormat(",
                          "void CGLReleasePixelFormat(", "CGLContextObj CGLRetainContext(", "void CGLReleaseContext(",
                          "CGLError CGLCreateContext(", "static CGLWindowRef create_window(", "CGLWindowRef CGLGetWindowForContext(",
                          "CGL_EXPORT void CGLDestroyWindow(", "CGL_EXPORT CGLError CGLContextMakeCurrentAndAttachToWindow(",
                          "CGLError CGLClearDrawable(", "CGLError CGLFlushDrawable(", "CGLError CGLSetParameter(", "CGLError CGLGetParameter("):
            gl_code += function(gl, signature)
        gl_code += gl[gl.index("static pthread_key_t current_context_key;"):gl.index("CGLError CGLSetFullScreen(")]
        write(temporary, "opengl-source.h", gl_code)
        run([*common, "-I", temporary, ROOT / "validation/opengl.c", "-o", temporary / "opengl"])
        run([temporary / "opengl"], env=environment)

        loader = (ROOT / "src/startup/mldr/elfcalls/threads.c").read_text()
        loader_code = loader[loader.index("typedef void (*thread_ep)"):loader.index("static void* darling_thread_entry(void* p);")]
        loader_code += '#define DEFAULT_DTHREAD_GUARD_SIZE 0x1000\n#define CIDER_DEFAULT_THREAD_STACK_SIZE (8UL * 1024 * 1024)\n'
        for signature in ("static dthread_t dthread_structure_init(", "static dthread_t dthread_structure_allocate(", "void* __darling_thread_create("):
            loader_code += function(loader, signature)
        write(temporary, "threads-source.h", loader_code)
        run([*common, "-I", temporary, ROOT / "validation/threads.c", "-o", temporary / "threads"])
        run([temporary / "threads"], env=environment)

        write(temporary, "string-include/platform/string.h", """
#include <stddef.h>
#include <stdint.h>
extern void *_platform_memmove(void *, const void *, size_t);
""")
        strings = ROOT / "src/external/libplatform/src/string/generic"
        run([*common, "-fno-builtin", "-D__FBSDID(x)=", "-DLONG_BIT=(__SIZEOF_LONG__*8)", "-I", temporary / "string-include",
             ROOT / "validation/strings.c",
             *[strings / (name + ".c") for name in ("memmove", "memchr", "memcmp", "strcmp", "strlen", "memset_pattern")],
             "-o", temporary / "strings"])
        run([temporary / "strings"], env=environment)

        register = (XNU / "src/xnu_syscall/bsd/impl/bsdthread/bsdthread_register.c").read_text()
        trampoline = write(temporary, "workqueue.c", "void *wqueue_entry_point;\n" +
                           function(register, "void wqueue_entry_point_asm_jump("))
        for optimization in ("-O0", "-O2"):
            run([compiler, optimization, "-fno-omit-frame-pointer", trampoline,
                 ROOT / "validation/workqueue.c", ROOT / "validation/workqueue.S",
                 "-o", temporary / "workqueue"])
            run([temporary / "workqueue"])
    print("CiderDarling source regressions passed", flush=True)


if __name__ == "__main__":
    main()
