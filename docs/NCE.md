<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NCE (native code execution)

## Index

* [The thread's CPU: TLS_SLOT_APP](#the-threads-cpu-tls_slot_app)
  * [Before Android 10](#before-android-10)
* [The guest's addresses: moved above 64 GiB](#the-guests-addresses-moved-above-64-gib)
* [The system counter: read in its stub](#the-system-counter-read-in-its-stub)
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

- The guest's addresses stay below `AS_MAX` (128 GiB), which the tables indexed by them are sized for, so they fit in
  39 bits. That leaves Android 64 GiB of address space at most (Linux has 128).
- The 36-bit address space (NPDM type 1, games of SDKs before 2.0) has to lie below 64 GiB with its code at 128 MiB,
  where ART is. Android recompiles those games ahead of time and says so; a `.nce` brought from Linux ends the game
  with the base it found. The NPDM has no ASLR bit to ask for fixed code otherwise (Eden reads bits 0 to 3 of its
  flags, the 64-bit bit and the address space type).
- The base is the first free one, so a phone gives the same base run after run. A save state from a run of another
  base holds addresses of that base.

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

- `nce_patch`, any host: the sites, the stubs in both forms, the `.nce` file read back from a moved base, and which
  games each host takes (`nce_refused`).
- `nce_host` and `nce_host_indirect`: guest code runs natively in the direct form and in Android's. They run on an
  AArch64 Linux host, or under qemu-aarch64 with `SNX_AARCH64_SYSROOT` (and `SNX_AARCH64_GCC_TOOLCHAIN`,
  `SNX_QEMU_AARCH64`). The indirect one also takes a word another library holds and checks the refusal. The counter's
  stubs read between `guest_ticks` before and after them, keep their scratch registers, count each read, and read
  where a pause stopped the clock.
- `guest_stats`, any host: a window's SVCs, misses, outcalls and families, a thread's time read from another thread,
  and the lines that say them.
- `memory_accounting`: the 48 GiB tier moved past a page held at 65 GiB, its regions, a mapping there, and a 36-bit
  layout refused (Linux, without ASan, whose shadow holds those addresses).

No test runs on an Android device: the arm64 objects build, and the rest is the phone's to show.
