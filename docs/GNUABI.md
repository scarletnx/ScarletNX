# Linux AArch64 GNU ABI runtime

## Index

* [Build and run](#build-and-run)
* [The sysroot](#the-sysroot)
* [How code runs](#how-code-runs)
* [Processes](#processes)
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
llvm-mingw toolchain and bootstrap environment described in [BUILD.md](BUILD.md).

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
([USAGE](USAGE.md#folders)): the portable folder beside a portable copy, the user's data
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
one code arena and its TOC instead, the runtime library loaded into it once ([PowerPC64](PPC64.md)).

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

Guest calls are host calls in the generated code, so the guest runs on a host thread
with a 1 GiB stack reservation.

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

Windows has no fork. A vfork child runs in its parent's process (with its own
descriptor table, registers and signal state) until it execs. Then another copy of
this runtime starts the program, adopting the child's descriptors as inherited
handles (`--fd=N:HANDLE`, `--cwd`), and the parent goes on from its `clone`. A child
that exits before its exec leaves a status for `wait4`. An `execve` outside a vfork
child starts the new program the same way and ends with its status. A plain `fork`
returns `ENOSYS` there. The Windows paths compile with llvm-mingw but have not been run.

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
`execveat`, `wait4`, `waitid`, `kill`, `tgkill`, `exit_group`, ids, process groups,
`umask`), `uname`, clocks and sleeps, random bytes, resource queries, signal action and
mask storage, and basic single-thread futex operations. Networking adds host-backed
sockets, socketpair, poll/select, epoll and eventfd through the shared stack;
read-only virtual `/etc` files support guest libc DNS. See [NETWORKING.md](NETWORKING.md)
for configuration, tests, and current semantic limitations. Guest structures and
constants are encoded for AArch64 rather than copied from the host ABI.

Each process runs one guest thread: `clone` of a thread (`CLONE_THREAD`) returns
`ENOSYS`. Signal handlers are recorded but never entered, so `rt_sigreturn` and
signal frames are unsupported. File-backed writable `MAP_SHARED` is unsupported. Futex
wake returns zero without peers; waits with a mismatched value return `-EAGAIN`, timed
waits can return `-ETIMEDOUT`, and a blocking wait without a timeout returns `-ENOSYS`.

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
