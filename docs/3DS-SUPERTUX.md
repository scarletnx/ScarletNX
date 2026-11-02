<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# SuperTux integration target

## Index

* [Pinned input (not distributed)](#pinned-input-not-distributed)
* [Reproduce](#reproduce)
* [Observed startup](#observed-startup)
* [Validation boundaries and next work](#validation-boundaries-and-next-work)

The supplied SuperTux CIA **reaches and executes `main`**, at `0x00167d88`
after **50,569 native blocks**. It reads its RomFS through NX's shared indexed
host filesystem. It also draws its title and loading screens
with the software PICA pipeline and plays its title music through the HLE DSP.
It is **not playable**: it has no input, only the top screen is shown, and the
music gaps after each rendered frame (see the [parity roadmap](3DS.md#parity-roadmap-azahar)).
There are no title-specific runtime hooks, patched instructions, interpreter,
runtime target discovery or runtime compilation.

## Pinned input (not distributed)

The supplied `d6nm2s3.zip` contains `Supertux.cia`, `Supertux/Supertux.3dsx`
and `Supertux/Supertux.smdh`. It is nop90's SuperTux Milestone 1 port, release v0.1a
(`Supertux_M1_v0.1a.zip` at github.com/nop90/Supertux-Milestone1-3ds), byte for byte.
This uses the CIA, not a new 3DSX loader.
Keep binaries, extracted assets and derived native modules outside git.

```text
82d2860b06325526c2cdf95c0dc4aa1364668a3a2509eea6f1a74c58a4d6eb01  d6nm2s3.zip
943d98144d6e7ffdbcf9cdbce43a4d0b4630afb8bc88f89d02ecc7f24eae5784  Supertux.cia
061c7021e97c2358da5795edeabb081f174e346472ccee82b57d6b56d9dda3e5  expanded exefs/.code
```

Title `0004000000998200` contains **834 extracted RomFS files** (images,
levels, music and sounds). They back a reconstructed CTR level-3 RomFS view;
the runtime does not reopen the original CIA/container.
Text starts at `0x100000`, with 340 pages / 1,389,676 declared bytes;
RO at `0x254000`, 54 pages / 217,776 bytes; data at `0x28a000`, four pages.
Raw data bytes `0x21ac0` include BSS `0x1e36c`; initialized bytes `0x3754`
fit those pages. BSS is not taken from input code. Stack size is `0x40000`;
expanded page-packed code is 1,630,208 bytes.

## Reproduce

A 64-bit Linux or Windows host, a supported Clang and an LLVM SDK are required.
LLVM 19.1.7 and 23 were tested; use `SNX_LLVM_CONFIG` for a non-system SDK.

```sh
cmake -S . -B build/ctr-aot \
  -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_GUI=OFF \
  -DSNX_TEST_ARM_AOT=ON \
  -DSNX_TEST_3DS_SUPERTUX=/absolute/path/to/Supertux.cia
cmake --build build/ctr-aot -j4
ctest --test-dir build/ctr-aot -R 3ds_supertux_bootstrap -V
```

A manual run (a finished extraction and module in the output folder are used again):

```sh
build/ctr-aot/scarletnx-3ds --output work/supertux-001 /path/to/Supertux.cia
```

A plain launch runs until the game exits or its window closes. `--max-blocks N`
stops it after N blocks with host status **3**, which `3ds_supertux_bootstrap` uses
for the driver (100,000 blocks). Its probe runs one block at a time until the title's
archive timestamp request is answered (within 500,000 blocks), then 50,000 blocks
more and four of the driver's polled slices. Coverage was **73,783 blocks / 578
objects** at the last count, statically compiled and linked before guest execution
(the compile prints both). Where a budgeted run stops depends on host-paced vblank
and DSP time, so its pc and SVC count vary.

Baseline, 2026-10-17, Windows 11 (MSYS2 CLANG64, LLVM 23.1.3), `--max-blocks 300000000`,
two runs on the same module:

- The run takes **151 s** for 300 million blocks and about 97,000 SVCs. Compiling the
  module first takes about 470 s more.
- It shows the title screen, with its menu, from about 22 s. The eight PPM snapshots
  are seven of the top screen and one of the bottom screen, which is black then.
- Music starts at about 22 s. Over the following 97 s, `audio.wav` has 574 gaps of 5 ms
  or more, 36.5 to 38.1 s of silence in all. The median gap is 54 ms and a gap comes
  every 161 to 164 ms (median), one per rendered frame. That puts a rendered frame at
  about 160 ms, roughly 6 frames per second, with the software renderer on the guest thread.

The external test independently observes the CRT load of main's address from
`0x10008c`, its `BX r3` at `0x100054`, entry at `0x167d88`, and CRT return
address `0x1626c4`. It then executes main through its RomFS opens and the archive
timestamp request, and on past them. This observation exists only in the test, not
the runtime. Block counts are not cycles.

A finished extraction and module are reused as the Switch driver reuses its own; the
module's identity is checked against the image, but its cache key does not yet cover the
compiler's version or configuration (parity roadmap F8). Native modules are trusted host
code, not sandboxed or signature-authenticated by ABI/hash checks.

## Observed startup

1. Guest entry clears BSS, creates a real arbiter (`0x201`), and reports
   `0x1ee000` initial application allocation on Old 3DS. The configuration page
   is read-only AXI WRAM outside that accounting.
2. Real ControlMemory commits `0x1f09000` heap bytes at `0x08000000`, then the
   same-size linear allocation at `0x160f7000`. Checked ARM11 exclusives execute
   startup locks, with alias/ABA/write/mapping reservation invalidation.
3. `srv:` connection (`0x202`) and generated client registration succeed.
   `APT:S` supplies a shared recursive mutex and two real one-shot events, in
   **one copied-handle descriptor**. Its first/only application receives the
   manager's initial empty Wakeup, not a fabricated Home Menu session.
4. The guest creates a native background thread with independent zeroed TLS
   and stack. Priority/blocking scheduling and event/mutex/arbiter wakeups are
   serialized; waits do not complete just because dispatch is called again.
5. The startup subset handles readiness, receiving the queued Wakeup, releasing
   its startup transition, and the virtual console's open-shell case. Unknown
   applets/transitions and closed-shell sleep remain unsupported.
6. PTM confirms the existing base CPU configuration; accelerated modes remain
   unsupported. APT/PTM model queries report the actual board profile.
7. `hid:USER` supplies a real read-only system-pool page and five real events.
   The page maps at `0x10000000`. No samples or completed input waits are faked;
   periodic/host input is not implemented in this increment.
8. FS initializes the caller and opens SDMC archive 9, a read-write folder for the
   title on the host, where ndsp finds the stand-in `3ds/dspfirm.cdc`. Constructors run using newly supported
   ARM11 `UADD8`/`SEL`; bounded adjacent-CMP/LDRLS jump-table discovery supplies
   the missing static constructor targets.
9. CRT enters `main`; FS `OpenFileDirectly` on archive 3 returns a genuine
   read-only file object over the process' reconstructed RomFS. Host nodes,
   shared mappings and fallback opens are the same backing implementation NX uses.
10. `gettimeofday` reads the shared page's date and time (the host's local time)
    and host-paced GetSystemTick (`0x28`), then executes scalar VFPv2
    conversions and arithmetic. The offline CFG follows VFP fallthrough, and no
    target is discovered at runtime.
11. `OpenArchive` on SelfNCCH returns an owned, FS-private 64-bit token backed
    by the process' real RomFS. `ControlArchive` GET_TIMESTAMP returns the
    backing file's real host modification time as a 3DS datetime (bounded
    UTF-16 path, `/` and `\` separators). A save commit on the SDMC area
    succeeds, and other actions stop.
12. `gsp::Gpu` supplies a real 0x1000 GX/interrupt shared block, relay threads
    and a host-paced 60 Hz display. GX DMA, memory fills, display transfers and
    texture copies run on the transfer engine, and PICA command lists draw through
    the software renderer. Vulkan-only presentation shows the top screen's
    landscape frame live in an SDL3 window and records bounded PPM snapshots in
    `<output>/frames`. The title screen and its menu show from about 22 s.
13. dsp::DSP takes ndsp's pipe writes and the HLE DSP plays the title music, heard
    through SDL3 and recorded to `<output>/audio.wav`.

See [CTR IPC](CTR-IPC.md) for the buffer and service policies. The full APT
lifecycle, applets, save data and extdata are not implemented. The SDMC archive
is read-write, with file creation, deletion, renames, directories and FSFILE
Write and SetSize. Archive tokens are not kernel handles.

## Validation boundaries and next work

`SNX_TEST_ARM_DIFF=ON` with pinned `unicorn==2.1.4` preserves three reference
boundaries: before the first SVC (30,949 blocks), before ConnectToPort (30,980),
and **before APT lookup handling** (31,018). All GPRs/CPSR and complete relevant
RAM snapshots match. Unicorn starts with the FPSCR the runtime gives the main
thread (`0x03C00010`). These block counts were measured before F1 and have not
been checked again since main's entry moved. The later two use test-scripted HLE/IPC effects: they prove
CPU consistency under those transcripts, **not independent hardware service
policy**. The runtime never invokes Unicorn or those scripted replies.
ARM11 packed-byte tests separately compare condition codes, GE flags and
register aliasing at O0/O2. Real service/kernel tests cover rollback and waits;
an asset-free native CIA proves a worker actually wakes and resumes main.
A second native CIA opens RomFS through real CTR IPC, reads its metadata and
resolves/verifies `hello.txt` from the extracted host tree before exiting.
`3ds_filesystem` independently parses Unicode names/hash chains/content and
checks both shared mappings and forced on-demand IO, EOF, failed-read atomicity,
whole-buffer RAM/MMIO/permission checks, quotas, rollback and handle lifetime.
Existing NX tree/service/raw-image fixtures run in CTR-only builds as well.

`arm11_vfp_differential` compares 52,480 exact instruction/state cases against
Unicorn's ARM11 MPCore at O0/O2, flat and checked, including registers, FPSCR,
conditions, aliases, DN/FZ and rounding modes. Checked callbacks forbid direct
pointer bypasses; negative tests verify fault rollback and unsupported controls,
legacy format-word saves and wide transfers. Native CTR tests cover atomic RAM
transfers and real FP helper linkage. The installed offline compiler embeds the
project's PIC semantic archive, stages it only for offline linking and needs no
build-tree archive lookup. Generated modules do not depend on LLVM. Default
A57 ARM/Thumb O0/O2 objects remain byte-identical. The public CPU, checked and
module ABIs are 3, 1 and 1 (`SNX_ARM_ABI`, `SNX_ARM_CHECKED_ABI`,
`SNX_CTR_MODULE_ABI`).

Next is F2 of the [parity roadmap](3DS.md#parity-roadmap-azahar): input, both
screens, rendering and presenting off the guest's thread so the music has no gaps,
and a playable title. Full VFPv2 (short vectors, exception traps), multicore
timing, broad capability policy and broader static coverage are still incomplete.
Preserve A57 defaults and the CPU and module ABIs.
