# Linux AArch64 GNU ABI runtime

## Index

* [Build and run](#build-and-run)
* [The sysroot](#the-sysroot)
* [How code runs](#how-code-runs)
* [Processes](#processes)
* [Threads and signals](#threads-and-signals)
* [Implemented scope](#implemented-scope)
* [Differential validation and fuzzing](#differential-validation-and-fuzzing)
* [ABI references](#abi-references)

ScarletNX runs Linux AArch64 programs, static or dynamically linked, with its ARM64
recompiler. `scarletnx-gnuabi` loads the ELF (and its dynamic linker from the sysroot),
compiles the guest's code into host code in its own process as execution reaches it,
and answers the program's Linux system calls. Processes the guest starts are host
processes, its pipes are host pipes, and its files are the sysroot's or the host's.
QEMU is an external reference used by tests; it is never the execution engine or a
dependency of this executable.

## Build and run

The GNU ABI runtime can be built without the Switch runtime or its graphics and
media packages:

```sh
cmake -S . -B build/gnuabi -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=ON
cmake --build build/gnuabi
ctest --test-dir build/gnuabi --output-on-failure
```

Both runtimes are enabled by default. The NX runtime always requires SDL3, Vulkan and FFmpeg.
Windows uses the existing
llvm-mingw toolchain and bootstrap environment described in [Build](../../dev/build.md).

```sh
build/gnuabi/scarletnx-gnuabi [options] [--] PROGRAM [arguments...]
build/gnuabi/scarletnx-gnuabi -L ~/aarch64-sysroot ./hello
```

PROGRAM is a host path. Arguments after it are passed to the guest, with the host's
environment (`-E VAR=VALUE` sets a variable and `-U VAR` removes one, as with QEMU).
`-0 ARGV0` replaces the program's `argv[0]`. `--help` lists every option.

| Option | Effect |
| --- | --- |
| `-L DIR`, `--sysroot=DIR` | The guest's root (see below) |
| `--isolated` | No host files: `/tmp` in memory and the program alone as `/program`, the namespace the unit tests use |
| `--jit` (default) | Compile the guest's code in process as it runs, the only mode |
| `--jit-cache=DIR`, `--no-jit-cache` | Where compiled code is kept between runs (default `linux-jit` in the user's cache folder) |
| `--jit-opt=LEVEL` | The JIT's LLVM optimization level (default 2) |
| `--verbose-jit` | A line for each piece of code compiled, and how long it took |
| `--strace` | Every system call, its arguments and its result on stderr, prefixed with the process id |
| `--sample-pc` | The guest's program counter on stderr once a second, for a loop that makes no system call (not on Windows) |
| `--host-page=N` | Work as a host with N-byte pages, a power of two from the host's page to 1048576 (not on Windows, tests) |

## The sysroot

The guest's file namespace works like QEMU's `-L`. A guest path names the file at that
path under the sysroot when the sysroot has one, and the host's file at that path
otherwise. The program's own path, its working folder (the host's) and files named on
its command line therefore work as they do natively, while `/lib/ld-linux-aarch64.so.1`,
`/usr/include` and `/usr/lib/libc.a` come from the sysroot. A file the guest creates is
made in the tree its folder is in.

Unlike QEMU, a path is resolved one component at a time, so a symlink in the sysroot
whose target is absolute (`lib64 -> /lib`, the linker scripts' neighbors) leads to the
sysroot's `/lib` rather than the host's, as under chroot. `/proc` and `/dev` are the
runtime's own (`/proc/self/exe` and `/proc/self/fd/N` lead to the host files they name),
as are the resolver files the network layer provides under `/etc`.

Without `-L`, the sysroot is `linux/` of the data folder `games/` and `work/` are in
([USAGE](../../user/usage.md#folders)): the portable folder beside a portable copy, the user's data
folder otherwise (`~/.local/share/scarletnx` on Linux), from a build tree too. The guest
path `/usr/lib/libc.a` is then `linux/usr/lib/libc.a`. Nothing in `linux/` is ever committed: it holds whatever
sysroot and guest toolchains are put there (`.gitignore`). A glibc sysroot such as
[gnu-cross](https://github.com/cross-tools/gnu-cross/releases)'s
`aarch64-unknown-linux-gnu/aarch64-unknown-linux-gnu/sysroot`, copied to `linux/`, is
enough to run dynamically linked programs and an AArch64-hosted GCC:

```sh
cp -a aarch64-unknown-linux-gnu/aarch64-unknown-linux-gnu/sysroot ~/.local/share/scarletnx/linux
build/scarletnx-gnuabi ~/.local/share/scarletnx/linux/opt/gcc/bin/aarch64-linux-gcc --sysroot=/ hello.c -o hello
build/scarletnx-gnuabi ./hello
```

The guest compiler's `--sysroot=/` points it at the guest's root, which is `linux/`.

## How code runs

One dispatch table covers the whole 4 GiB guest address space. A guest address the table has no host function for
sends the generated code to the runtime, which compiles the code there with the LLVM backend (the recompiler
library's `snx_recompile_from`) and runs it. One step is the function execution reached and the functions it calls,
breadth first, up to 64Ki instructions, split into pieces of at most 4096 instructions (LLVM's time grows faster than
a function's size) and compiled on every core. Each step is a module: its pieces, the descriptor that fills the
table, and the runtime library, linked into a shared object and loaded with `dlopen`. On PowerPC64 the steps share
one code arena and its TOC instead, the runtime library loaded into it once ([PowerPC64](../../hosts/ppc64.md)).

Compiled modules are kept in the cache, named by a hash of the code, its addresses and
entries, the backend's settings, `SNX_ABI` and the host CPU, together with the compiled
runtime library every module links. A program run again (or a toolchain run on another
file) loads instead of compiling. The first run of a large program is mostly compiling:
the first compile of a C file with GCC (`cc1`, `as`, `collect2`, `ld`) spent about seven
minutes compiling on six cores, and the same compile then took 6 seconds.

`mmap`, `munmap` and `mprotect` over executable pages drop their compiled code, and a
syscall that changed the code it returns into resumes through the dispatch table.
Code on a writable and executable page is compiled one function at a time, so its calls
go through the table, and the page is write-protected on the host: a write to it drops
its code, and the store is retried. `tests/gnuabi/self_modify.S` and `rewrite.S` exit
42 in both modes. Fetching from a page without execute permission faults.

Guest calls are host calls in the generated code, so each guest thread runs on a host
thread with a 1 GiB stack reservation.

The guest's threads share the dispatch table, the memory map and the descriptor table. One
compile runs at a time (the JIT's lock), and a thread that misses code another thread
compiles waits for it. The table's slots are published with release stores and read with
relaxed loads, as generated code reads them. The memory map changes under its own lock; a
guest access reads the page flags without it. A system call holds the descriptor table's
lock, and lets it go around a host call that can block (a pipe, a terminal, a socket), with
a reference that keeps the open file.

## Processes

On Linux and the other POSIX hosts, `clone` is the host's `fork`: the child has a copy
of the guest's memory, registers and descriptor table, and its descriptors share the
host's open files with the parent's, as Linux shares them. `CLONE_VFORK` (`vfork`,
`posix_spawn`) waits in the parent until the child has exec'd or exited, as the kernel
does. The child has a copy of the memory rather than sharing it, which is all a vfork
child may rely on before `execve`. `execve` loads the new program into the same host
process, keeping its pid, its descriptors without the close-on-exec ones, and the
signals it ignores. `#!` scripts run their interpreter, found through the sysroot.
`wait4`, `waitid`, `kill`, `tgkill`, process groups and sessions are the host's, with
signal numbers and wait statuses translated. A guest that ignores a signal has the
host ignore it (`SIGPIPE`, `SIGCHLD`, `SIGINT`). A signal whose default action ends a
process ends the host process with the same signal, so a parent sees what it would on
Linux.

A fork from a thread copies that thread alone, as on Linux. Every lock of the runtime
(descriptors, memory map, JIT, futexes, process) is held across the host's fork, and the
child makes each one again, since its thread has a new id.

Windows has no fork. A vfork child runs in its parent's process (with its own
descriptor table, registers and signal state) until it execs. Then another copy of
this runtime starts the program, adopting the child's descriptors as inherited
handles (`--fd=N:HANDLE`, `--cwd`), and the parent goes on from its `clone`. A child
that exits before its exec leaves a status for `wait4`. An `execve` outside a vfork
child starts the new program the same way and ends with its status. A plain `fork`
returns `ENOSYS` there. The Windows paths compile with llvm-mingw but have not been run.

## Threads and signals

The threads, futexes and signals are a library of their own, `scarletnx-gnuabi-kernel`
(`src/gnuabi/kernel/`, with `kernel.h` as its interface). It has no loader, file system
or descriptor table. The runtime that uses it gives it the guest's memory (a checked read
and write, and the host address of guest address 0), and a way to stop a thread's host
call, end the process and leave a thread. This runtime and the Android bridge both use it.

The library also answers the system calls that need no loader, file system or descriptor
table, with Linux's AArch64 behavior and the checked memory access: `clock_gettime`,
`clock_getres`, `gettimeofday`, `times`, `getrusage`, `sysinfo` (the host's memory on
Linux), `uname`, `getrlimit`, `setrlimit`, `prlimit64`, `prctl` (`PR_SET_NAME`,
`PR_GET_NAME`, `PR_SET_VMA`, `PR_SET_DUMPABLE` and the other options of bionic and the C
libraries; `EINVAL` for the rest, and for SVE, pointer authentication and tagged
addresses), `getcpu`, `getpid`, `getppid`, the user and group ids, `getgroups`,
`setpriority`, `getpriority`, the `sched_*` parameter calls, `madvise` (checked, then the
runtime's), `getrandom`, `membarrier` (the host's barrier on every thread: Linux's
`membarrier`, Windows' `FlushProcessWriteBuffers`; `ENOSYS` on macOS, FreeBSD and a Linux
that refuses it) and `rseq` (`ENOSYS`). The
Android bridge sends `syscall()` and the bionic functions that are one system call to it.

**Threads.** `clone` with `CLONE_THREAD` (it needs `CLONE_SIGHAND` and `CLONE_VM`) starts
a host thread with the caller's registers, the new stack, `CLONE_SETTLS`,
`CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID` and `CLONE_CHILD_CLEARTID`. Both tid words are
written before either thread goes on. A thread's tid is its host thread's id on Linux, so
`tgkill` from another process reaches it. At a thread's exit the runtime walks its robust
list (`set_robust_list`), writes 0 to its `clear_tid` word and wakes one waiter there, as
Linux does. `exit` ends one thread, and the last thread's `exit` ends the process.
`exit_group`, a fault and a fatal signal end the process: the other threads leave their
guest code at their next delivery point, and a thread blocked in a system call is not
waited for. `execve` from any thread first stops all the others, and the thread takes the
process's id. `gettid`, `set_tid_address`, `tkill`, `tgkill`, `sched_yield`,
`sched_getaffinity` (the host's processors) and `sched_setaffinity` (accepted) are there.

**Futexes.** `FUTEX_WAIT`, `WAKE`, `REQUEUE`, `CMP_REQUEUE`, `WAKE_OP`, `WAIT_BITSET` and
`WAKE_BITSET`, with the results Linux gives. A waiter reads the word and joins the list
under one lock, so a wake after a change of the word finds it. A relative timeout is on the
monotonic clock, and `WAIT_BITSET`'s absolute one on the monotonic clock or, with
`FUTEX_CLOCK_REALTIME`, on the realtime clock (converted once: a change of the realtime
clock during the wait does not move it). `FUTEX_PRIVATE_FLAG` changes nothing: guest
memory is never shared with another process. The PI operations return `ENOSYS`.

**Signals.** Each thread and the process have a queue: one entry of a standard signal, an
entry for each real-time one sent (up to 64 in a queue, then `EAGAIN`). A signal to the
process goes to its main thread if it does not block it, else to another thread that does
not. The sender sets the target's poll word, wakes its wait in the runtime (futex, sleeps,
`rt_sigsuspend`), ends its wait in the network stack (a wake pipe that its polls watch)
and sends its host thread a host signal (`SIGRTMIN+2`), which stops another blocking host
call with `EINTR`.

A signal is delivered only where all the guest's registers are in its CPU structure: at
`RtAPI.poll` (each backward branch and dispatch of generated code) and at the end of each
system call. Never in a host signal handler. The frame is Linux's `rt_sigframe`
(`src/common/runtime/linux_signal.c`, which the Android bridge shares), written with the
runtime's checked access; a frame that does not fit ends the process with `SIGSEGV`. The
handler returns to `sa_restorer` with `SA_RESTORER`, else to the runtime's page at
`0xfffff000`, which holds `mov x8, #139; svc #0` as the vDSO's `__kernel_rt_sigreturn`.
`rt_sigreturn` restores the registers, the mask and the alternate stack from the frame, and
the guest goes on at the restored pc through the dispatch table. `SA_SIGINFO`,
`SA_ONSTACK`, `SA_NODEFER`, `SA_RESETHAND` and `SA_RESTART` work, and a handler's changes to
its `ucontext` are the state it returns to.

A call that a signal stops follows Linux's restart rules: it restarts after a handler with
`SA_RESTART` (`ERESTARTSYS`), and fails with `EINTR` without one. `nanosleep`,
`clock_nanosleep`, `poll`, `ppoll`, `select`, `pselect6`, `epoll_wait`, `rt_sigsuspend`,
`rt_sigtimedwait` and a futex wait with a timeout never restart after a handler. A call
that a signal stops without a handler (the signal went to another thread) restarts.
`ppoll`, `pselect6` and `epoll_pwait` apply their mask while they wait, as `rt_sigsuspend`
does. `rt_sigsuspend`, `rt_sigtimedwait`, `rt_sigpending`, `sigaltstack`, `rt_sigqueueinfo`
and `rt_tgsigqueueinfo` are there.

With `SIG_DFL` the signal's default action runs: it is ignored, it stops the host process
(`SIGSTOP` to itself), or it ends the host process with the same signal, so a parent sees
what it would on Linux. A guest that handles a signal has the host relay it: the host
handler writes the signal to a pipe, and a relay thread sends it to the process. A guest
that ignores one has the host ignore it.

Limits:

* Faults in guest code (`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGTRAP` of `BRK`) end the process.
  Their registers are not exact at the fault (generated code keeps registers in host
  registers), so no guest handler is entered for them. `kill` of the process with
  `SIGSEGV` does run a handler.
* A host signal that comes after a thread entered a system call but before its host call
  blocks is lost for that call, which then blocks until it ends on its own. Waits in the
  runtime and in the network stack have no such window. A pipe or terminal read can.
  QEMU's `safe_syscall` closes it with a check of the pc in the host handler.
* A real-time signal to another process, and one from another process, goes through the
  host's numbers only for 1 to 31.
* macOS has no real-time signals, so the runtime's own host signal there is `SIGUSR2`: a
  `SIGUSR2` from another process does not reach the guest. The guest's own `SIGUSR2`
  (`kill`, `tgkill` and `rt_sigqueueinfo` of the process) and its `sigaction` work as on Linux.
* Signals for 32-bit guests (A32, Thumb) are not delivered: their calls that a signal stops
  fail with `EINTR`. They need a frame of AArch32 (`sigframe` with `ucontext` and the VFP
  registers, `sigreturn` 119 and `rt_sigreturn` 173, `SA_RESTORER` or a trampoline in A32
  or Thumb state, as the handler's bit 0 says), and delivery at the ARM block loop's points.
* `epoll_ctl` of another thread during an `epoll_wait` is seen at the next wait. A
  descriptor that another thread closes during a call on it (a socket's call, `ppoll`,
  `pselect6`, `epoll_pwait`) keeps its file open until the call ends, as Linux's file
  reference does.
* Windows: no host signals. A thread in a blocking host call (outside the network stack) is
  not stopped by a signal. A vfork child runs in its parent's process in place of the
  calling thread; the other threads go on.

## Implemented scope

The independent ELF loader accepts ELF64 little-endian `EM_AARCH64` `ET_EXEC`
and `ET_DYN`, validates program headers and load intervals, maps `PT_LOAD` with
their permissions, zeroes BSS, and creates an aligned stack containing `argc`,
`argv`, `envp`, and Linux auxiliary vectors. A position-independent program loads at
0x10000000. `PT_INTERP` names the dynamic linker, which is loaded from the guest's
namespace at 0x38000000 (`AT_BASE`), and the program starts there. This is a bounded
4 GiB guest address space with executable load segments below 1 GiB and an 8 MiB
initial stack. Dynamically linked AArch32 programs are unsupported.

`SVC #0` dispatches syscall number `x8`, arguments `x0` through `x5`, and result
`x0`. Failed calls return negative Linux errno values. Other SVC immediates,
undefined instructions, and guest breakpoints terminate with a diagnostic.
Unsupported syscalls return `-ENOSYS`.

Implemented syscall families cover file descriptors (`read`, `write`, vectored
I/O, `openat`, `close`, `lseek`, `dup`, `dup3`, `fcntl`, `pipe2`, `ioctl` for terminals and
`FIONREAD`), metadata and directory operations (`fstat`, `newfstatat`, `statx`,
`readlinkat`, `getdents64`, `mkdirat`, `unlinkat`, `renameat2`, `linkat`, `symlinkat`,
`fchmodat`, `fchownat`, `utimensat`, `faccessat2`, `truncate`, `statfs`, `fsync`,
`chdir`, `fchdir`, `getcwd`), memory (`brk`, `mmap` with private file mappings and
read-only shared ones, `munmap`, `mprotect`), processes (`clone`, `execve`,
`execveat`, `wait4`, `waitid`, `exit`, `exit_group`, ids, process groups,
`umask`), threads, futexes and signals (see [Threads and signals](#threads-and-signals)),
`uname`, clocks and sleeps, random bytes and resource queries. Networking adds host-backed
sockets, socketpair, poll/select, epoll and eventfd through the shared stack;
read-only virtual `/etc` files support guest libc DNS. See [Networking](../nx/networking.md)
for configuration, tests, and current semantic limitations. Guest structures and
constants are encoded for AArch64 rather than copied from the host ABI.

File-backed writable `MAP_SHARED` is unsupported.

The virtual nodes:

| Path | Contents and behavior |
| --- | --- |
| `/dev` | `null`, `zero`, `random`, `urandom`, and descriptor links under `fd` |
| `/proc` | Guest `self/exe`, `self/maps`, `self/cmdline`, `self/status`, `self/fd`, and AArch64 `cpuinfo` |
| `/tmp` | `--isolated` only: ephemeral in-memory files and directories. Otherwise the sysroot's or the host's |

No Maxwell, Vulkan, OpenGL, DRM/KMS, display, or graphics service is initialized
by this runtime.

## Differential validation and fuzzing

Install `qemu-aarch64` and an AArch64 cross compiler outside ScarletNX, then run:

```sh
python3 tools/gnuabi_diff.py --scarletnx build/gnuabi/scarletnx-gnuabi \
    --suite all --fuzz 100 --seed 1000
python3 tools/gnuabi_diff.py --scarletnx build/gnuabi/scarletnx-gnuabi \
    /tmp/libc-aarch64
```

The fixtures are freestanding static ELF files. The CPU suite covers arithmetic,
branching, unaligned memory, floating point, SIMD, exclusive atomics, a function
crossing a 4 KiB instruction boundary, and initial stack alignment. The syscall
suite exercises tmpfs, devfs, procfs, descriptor and metadata calls, clocks,
random reads, and unsupported-call errors. The PIE suite verifies ET_DYN,
PC-relative global accesses, and BSS initialization. The optional `libc.c` fixture exercises
static libc startup, allocation, memory operations, and formatted output.
The `threads` suite (`threads.c`, `signals.c`, `restart.c`, `sigsuspend.c`, `wake.c` and
`close.c`, with `thread_start.S`) covers threads, futexes, signal frames, `rt_sigreturn`,
`SA_RESTART`, `rt_sigsuspend`, a network wait after a signal and a descriptor closed during
a call on it; CTest runs it as `gnuabi_threads` (skipped without Clang and LLD).
The standalone `self_modify.S` fixture must exit 42 after replacing a compiled
instruction, as must `rewrite.S`, which rewrites code that already ran;
`fetch_fault.S` must fault after removing the executing page's execute permission. These intentional faults should be checked directly rather
than passed to the default differential success suite.

Seeded fuzzing generates unsigned integer and memory programs with reproducible
source; it is an initial corpus generator rather than complete ISA coverage.
Each generated fixture must exit zero. Existing input ELFs may intentionally
return another status. The tool compares exact stdout bytes and exit status,
detects timeouts and crashes, and optionally compares stderr with
`--compare-stderr`. Avoid comparing printed timestamps, process IDs, random
values, filesystem enumeration order, or host-specific paths. Tests should
assert their properties and print deterministic results instead.

Failures are retained under `build/gnuabi-failures` by default. Each case includes
the ELF, available source and seed, stdin, both output streams, compiler command,
ELF SHA-256, timings, original commands, and replay commands in `result.json`.
`--keep-success` also retains matching cases. Use `--timeout` to allow host
compilation of larger ELFs and `--compile-timeout` for fixture cross compilation.

Clang plus LLD can build the freestanding suites without a target libc:

```sh
python3 tools/gnuabi_diff.py --scarletnx build/gnuabi/scarletnx-gnuabi \
    --suite all --cc clang --cc-arg=--target=aarch64-linux-gnu \
    --cc-arg=-fuse-ld=lld --qemu /path/to/qemu-aarch64
```

Source-boundary and transitive CMake dependency checks run as
`runtime_architecture` in CTest. The harness unit tests intentionally create an
output mismatch and verify that a reproducible ELF and both results are saved.

## ABI references

The implementation uses the official
[Linux arm64 UAPI headers](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/arm64/include/uapi/asm),
[asm-generic syscall numbers](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/asm-generic/unistd.h),
and [Linux UAPI definitions](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi).
The [arm64 syscall entry](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/arm64/kernel/syscall.c)
also defines the syscall number's 32-bit conversion from `x8`.
Behavior is cross-checked with
[QEMU Linux user-mode documentation](https://www.qemu.org/docs/master/user/main.html)
and its [AArch64 syscall entry](https://gitlab.com/qemu-project/qemu/-/blob/master/linux-user/aarch64/cpu_loop.c).
