<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NCE (native code execution)

## Index

* [The thread's CPU: TLS_SLOT_APP](#the-threads-cpu-tls_slot_app)
  * [Before Android 10](#before-android-10)
* [The guest's addresses: moved above 64 GiB](#the-guests-addresses-moved-above-64-gib)
* [The system counter: read in its stub](#the-system-counter-read-in-its-stub)
* [An SVC outside the analysed code: the net](#an-svc-outside-the-analysed-code-the-net)
* [The runtime's calls into the game's code](#the-runtimes-calls-into-the-games-code)
* [Against the recompiled code](#against-the-recompiled-code)
* [Tests](#tests)

On an AArch64 host the game's 64-bit code can run on the host's CPU (`src/nx/runtime/nce/`, the setting `nce`). The
recompile makes no code ahead of time: it writes `<game>.nce`, the words to patch (SVC, the TLS and counter
registers, the exclusives) and the pools for their stubs. The runtime maps the guest at the host's own addresses and
patches the code after the load. [Architecture](./ARCHITECTURE.md) has the flow; this page has the two design calls
the Android port rests on, the counter's stub, their limits, and how a run measures it.

| Host | NCE |
|---|---|
| AArch64 Linux | 64-bit games, both address spaces, at the guest's own addresses |
| Android 10 and later, arm64 | 64-bit games of the 39-bit address space, moved up (below) |
| Android 9 and earlier, other hosts, 32-bit games | none: the recompile makes the code ahead of time and says why |

## The thread's CPU: TLS_SLOT_APP

A stub reaches the thread's `CPU` from `TPIDR_EL0` at a fixed offset (`NceEnv.tls`). On Linux that is static TLS of
the executable. Android's runtime is `libscarletnx.so`, a library the app loads: built for API 21 its TLS is emulated
(`__emutls_get_address`, no offset at all), and from API 29 bionic refuses an initial-exec access in a loaded
library. The word the stubs read is bionic's `TLS_SLOT_APP` (`TPIDR_EL0` + 16), which the app owns from Android 10
(API 29). It holds a pointer to the thread's block, so the stubs load twice (`NceEnv.indirect`) in the same three
words, and the outcall's asm does the same (`nce_host.c`).

`TPIDR_EL0` stays the host's while guest code runs. ART's signal handlers, reached through libsigchain, and the
runtime's own (the write tracking's `SIGSEGV`, NCE's kicks and traps) run C code as they do without NCE, with bionic's
TLS where they expect it.

Limits:

- One word for the whole process: another library that uses `TLS_SLOT_APP` breaks the stubs. The runtime checks the
  word is 0 before it reserves anything (`nce_host_check`): if not, NCE is off, the log names the value found, and the
  JIT compiles the game's code. Each thread checks again as it starts guest code (`nce_host_attach`), and a word
  taken there ends the game with the value. A library that writes the word while the runtime serves a guest thread
  (an outcall) is not seen.
- Android 9 and earlier keep `errno` in that word: no NCE, with the API level in the recompile's line.

### Before Android 10

What would serve them is Eden's way (`src/core/arm/nce/arm_nce.s`): `TPIDR_EL0` set to the thread's NCE block while
guest code runs, and the host's put back on each outcall. It needs no slot, so it works from API 21. The cost:

- Each signal that lands in guest code has to restore `TPIDR_EL0` in asm before any C, the write tracking's `SIGSEGV`
  among them, which is a hot path.
- ART's libsigchain runs first and reads bionic's TLS: Eden installs its handlers with libc's own `sigaction`
  (`common/signal_chain.cpp`) to stay ahead of it. Every handler of the runtime would need that.
- The guest's `MRS TPIDR_EL0` already goes through a stub, so the stubs stay as they are; the outcall and the four
  handlers change.

## The guest's addresses: moved above 64 GiB

The guest's addresses are the host's. An app's process holds the low 4 GiB (ART's heap, whose references are 32-bit,
and its boot image), where the guest's code region lies at 128 MiB. On Android the whole layout moves up: the
reservation takes the first base from 64 GiB, in 2 MiB steps, where a tier of the address space is free
(`mem_reserve_identity`, as Eden takes its base above 36 bits in `host_memory.cpp`). Every region, the module's
addresses (`nce_module`) and the `.nce` file's (kept from the first module's base) move with it. The log names the
base: `the code region from ...`.

Limits:

- The guest's addresses stay below `AS_TOP` (512 GiB), which the tables indexed by them are sized for, so they fit in
  39 bits. NCE's address space is at most the console's 128 GiB layout (`AS_MAX`), its 64 GiB alias region among
  them, which Android's moved layout keeps from any base up to 384 GiB; where none of those is free a smaller tier
  takes it, with a smaller alias region (32 GiB in 64, 16 GiB in 48). Without NCE the game gets 512.
- The 36-bit address space (NPDM type 1, games of SDKs before 2.0) has to lie below 64 GiB with its code at 128 MiB,
  where ART is. Android recompiles those games ahead of time and says so; a `.nce` brought from Linux ends the game
  with the base it found. The NPDM has no ASLR bit to ask for fixed code otherwise (Eden reads bits 0 to 3 of its
  flags, the 64-bit bit and the address space type).
- The base is the first free one, so a phone gives the same base run after run. A save state from a run of another
  base holds addresses of that base.
- A crash report places the fault's address against the layout (`mem_place`): in it, below its base (an address the
  move left out) or past its end (one a layout smaller than the console's 512 GiB does not hold), and how far.

The guest's addresses share the process with ART's heap and JIT, the allocator and the GPU's driver. The reservation
holds them all, so none of those is given one by an mmap of its own; one that maps over guest memory at a fixed
address of its own would make memory the game's code reads and writes as its own. As the game's code starts, the log
names anything there that is not the game's (`mem_foreign`: a mapping with a name, or a run nothing maps; the alias
pool and a GPU's buffer that guest memory moved into are the runtime's), and a crash report names it again. With
`nce_foreign_scan=N` (Debug, seconds) the kicks' thread looks again every N s while the game runs and logs each new
finding once; off, it reads the setting and nothing else (`nce_foreign_look`). A plain anonymous mapping of another
owner reads as the game's, so this diagnoses and guards nothing. The maps are read while other threads change them: a
run they list as empty is asked again (`msync`) before it is named, and a failed read of them is no answer.

The path for the 36-bit games is to run the runtime outside ART: an executable child of `GameActivity`, as the driver
already is (`libscarletnx-run.so`), whose low addresses are free. The window would come from the app over a socket
(`AHardwareBuffer`). Built for API 29, such an executable could also take static TLS.

## The system counter: read in its stub

The SDK's system tick is the game reading the counter register itself (`MRS CNTPCT_EL0` or `CNTVCT_EL0`), which a game
that times its work closely does often. Its stub makes no outcall: it reads the host's virtual counter after an `ISB`,
as Linux's vDSO reads it, makes it nanoseconds with a 64.64 factor of the host's frequency (Eden's `WriteCntpctHandler`
scales it the same way), takes away the guest clock's start (or reads where a pause stopped the clock), and makes
19.2 MHz ticks as `guest_ticks` does (`runtime/clock.c`). So that these reads and `svcGetSystemTick`, which still goes
through an outcall, never go back against each other, guest time under NCE follows the host's counter too
(`clock_from_counter`), from where it was when NCE started, in the stub's arithmetic.

Limits:

- Guest time then follows the counter, not `plat_ns` (SDL's clock). They drift apart by the kernel's rounding of the
  counter's frequency, which was not measured; what puts guest time beside `plat_ns` (`g_t0_ns`) sees that drift.
- The stub is 176 bytes, an outcall's 56. A `.nce` of version 2 has pools sized for the outcall, so it is refused
  and the game has to be recompiled.

## An SVC outside the analysed code: the net

The patcher changes the words that the recompiler's analysis finds as code, and the unproven ones inside its
functions. A word of those encodings outside every function keeps its bytes, so a literal of the same value stays as
it is (Eden patches every such word of `.text`). If one of them is code after all, a function only a table reaches,
an SVC there would run as a system call of the host, `x8` its number. Each guest thread takes a seccomp filter as it
starts the game's code (`nce_host_net`, `nce_net_install`, its words from `nce_net_filter`): a system call it makes
from the guest's addresses traps with `SIGSYS`, is not made, and the game stops with the SVC's address and immediate.
The runtime's own threads make their system calls with no filter of ours. The recompile's line counts the
words outside the functions by kind.

Limits:

- Every system call of a guest thread runs the filter's 14 words first, and so do those of a thread a guest thread
  starts (a filter is inherited, and a runtime thread made during an outcall inherits it too). What that costs was not
  measured. A thread that has the net from the one that started it takes no second one (its count of filters, read
  from `/proc/thread-self/status`, is past the runtime's); before Linux 5.9, which does not give that count, it
  stacks a second.
- The filter needs `PR_SET_NO_NEW_PRIVS`, which is set on each guest thread alone: a setuid program it starts gains
  nothing, and the runtime's threads keep theirs.
- The other kinds outside the functions (the TLS and counter registers' MRS) read the host's registers, which no
  filter sees: only the recompile's count shows them.
- A kernel without seccomp filters, and qemu-user, which has none, run without the net and say so in the log.
- An app's process on Android has the system's filter already, and ours stacks on it: only a phone shows it does.

## The runtime's calls into the game's code

The runtime calls the game's code itself where the console's driver calls it back: an NVN command buffer's memory
callback, when its memory runs out (`nvn_cmd.c`'s `guest_call`). No dispatch slot holds the game's code under NCE and
no JIT runs, so `snx_lookup` misses; `RtAPI.miss` then runs the code natively from its pc, as an empty slot does
(`nce_slot`), in a run nested in the outcall that called NVN. It returns to the caller's link register, and the
runtime goes on from there.

## Against the recompiled code

The renderer's first present and every 600 after log what the game's threads cost the host (`runtime/guest_stats.c`),
the same whichever way the code runs, so runs with `nce=1` and `nce=0` (each recompiled with its value) over one scene
compare window by window. `cpu:` gives the game's threads' time on the host's processors, in all, per present and for
the busiest, beside the whole process's (native code changes the first; the frame rate only when the processors set
it), then the SVCs by number and the calls the dispatch table had no code for (RtAPI.miss). Under NCE, `nce:` gives
the outcalls by kind (SVC, a hooked function's by family, a native run's return), the counter reads that their stubs
made with no outcall, the native runs started and the kicks: an outcall saves and loads the guest's registers, and a
run or a kick is a signal. Each thread adds to counters of its own, with no lock.

## Tests

- `nce_patch`, any host: the sites, the stubs in both forms, the `.nce` file read back from a moved base, which games
  each host takes (`nce_refused`), a module of 200 MiB laid out with a pool before it and one after (and 300 MiB
  refused: the recompile ends, naming the word, and says to set `nce=0`), and the net's filter at the edges of its
  range, by an interpreter and, on Linux, by the kernel in a child: under a guest thread and the thread it starts, not
  under the runtime's thread or one it starts after.
- `nce_host` and `nce_host_indirect`: guest code runs natively in the direct form and in Android's. They run on an
  AArch64 Linux host, or under qemu-aarch64 with `SNX_AARCH64_SYSROOT` (and `SNX_AARCH64_GCC_TOOLCHAIN`,
  `SNX_QEMU_AARCH64`). The indirect one also takes a word another library holds and checks the refusal. The counter's
  stubs read between `guest_ticks` before and after them, keep their scratch registers, count each read, and read
  where a pause stopped the clock. Under the net, an SVC that no stub holds stops in the runtime with no system
  call made (a host with seccomp filters: qemu-user skips it).
- `rtapi_miss`, any host: a pc with no code runs the game's native code under NCE and returns to the caller's link
  register, asking no JIT; without NCE the JIT's code runs.
- `nce_module`, any host: the modules moved with the layout and the main thread's entry in them; and the kicks' look
  at the guest's addresses, none with `nce_foreign_scan=0`, one every N seconds of rounds with N (1, 10 and 3600),
  each finding said once.
- `guest_stats`, any host: a window's SVCs, misses, outcalls and families, a thread's time read from another thread,
  and the lines that say them.
- `memory_accounting`: the 128 GiB tier moved past a page held at 65 GiB, its regions, a mapping there, where a crash's
  address lies against it, a named mapping over guest memory a GPU's buffer holds taken as the runtime's, and a 36-bit
  layout refused (Linux, without ASan, whose shadow holds those addresses).
- `platform_posix`, Linux: what of the process is not the guest's in a range (`plat_vm_foreign`), against the real
  kernel: another owner's memfd over it, a run given back and a named anonymous mapping are named, the alias pool's
  pages and a mapping the caller claims are not, a failed read of the maps is no answer, and a run they list as empty
  that is mapped when asked again is no gap.

No test runs on an Android device: the arm64 objects build, and the rest is the phone's to show.
