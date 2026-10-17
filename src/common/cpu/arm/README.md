# Cortex-A57 A32 LLVM backend

This is an initial ARMv8-A AArch32 **user-mode CPU backend**, not an AArch32 guest OS or a complete Cortex-A57 emulator. It emits native LLVM IR, not calls to an instruction interpreter. The `snx_arm_llvm` CMake target is built alongside, and linked by, the existing LLVM backend.

The NX runtime runs 32-bit programs with it. The recompiler compiles a 32-bit program's blocks ahead of time into its game module (`src/nx/loader/module_arm.c`, an `SnxArmModule`), and the runtime's threads run them one after another (`src/nx/runtime/arm.c`), compiling a block it reaches that the recompiler didn't find and recording it in the hints file for the next recompile. Their SVCs go through the same handlers as 64-bit ones, with the registers moved into the 64-bit SVC ABI's places (`snx_svc32` in `src/nx/hle/svc/svc.c`). The GNU ABI runner (`src/gnuabi/runtime/arm_exec.c`) and the recompiler library (`snx_recompile32`) compile blocks on first use. Never pass an A64 `CPU*` to an A32 block.

## Files and API

- `arm.h`: separate, versioned `SnxArmCPU` state and exit reasons, the backend's `SnxArmRt`, plus the Thumb decode model.
- `rt_helpers.c`: the runtime library's calls into the backend (`snx_arm_undefined`, `snx_arm_coproc`), with the defaults for no backend.
- `a32.c`: validated A32 decoder and explicit libgccjit unsupported stub. No LLVM dependency.
- `thumb_decode.c`: validated Thumb16/Thumb32 decoder to an explicit operand model.
- `llvm.h`, `llvm.c`, `llvm_vector.inc`, `llvm_thumb.inc`: `snx_arm_llvm_block(module, name, words, count, pc, &consumed)` and `snx_thumb_llvm_block(module, name, halfwords, count, pc, &consumed)` emit `void name(SnxArmCPU*)` into a **host-target** LLVM module.
- `tests/arm_emit.c`, `tests/thumb_emit.c`: object generators demonstrating module creation, verification, optimization and native object emission.
- `tests/arm_test.py`, `tests/arm_vector_test.py`, `tests/thumb_test.py`: native execution witnesses and optional independent Unicorn comparisons.

Input words are host integers decoded from little-endian guest code (A32 words, or Thumb halfwords). A block consumes at most `count` instructions (or halfwords for Thumb) and stops at a control transfer, including a conditional transfer. `consumed` reports the compiled length. Bad input returns NULL before changing the module.

Initialize `r[15]` to the block's entry address, `cpsr` to user mode (`0x10`), and `mem`/`mem_size` to a flat guest mapping. Zero the exclusive monitor. The memory base must be at least eight-byte aligned. `mem_size` describes actual accessible storage, not a virtual reservation containing inaccessible holes. The mapping and CPU state must not overlap. Initialize the TLS fields from the guest thread. No A64 `SNX_ABI` bump is needed because this state is not part of that ABI.

On return:

- `exit == SNX_ARM_OK`: `r[15]` is the next instruction address. Look up its block and check `cpsr & SNX_ARM_T` before dispatching.
- SVC/BKPT: `syndrome` contains the immediate and `r[15]` points past the instruction. The caller handles the event.
- Undefined instruction: `syndrome` contains the instruction word. The PC stays at the faulting instruction.
- Data/alignment abort: `syndrome` is the address. The PC stays at the faulting instruction. Bounds checks precede memory accesses and register writeback.
- Bad entry or unsupported instruction state: execution stops explicitly.

`r[15]` stores the actual PC. Operand reads synthesize PC+8 in A32 (PC+4, word-aligned, in Thumb). Branches, ALU writes to PC and loads to PC implement AArch32 interworking and preserve the Thumb state bit. An A32 block rejects a Thumb entry state (and vice versa) as a bad entry: the dispatcher selects the block form from `cpsr & SNX_ARM_T`. Interworking branches set or clear `cpsr & SNX_ARM_T` before returning so the dispatcher can pick the next block's ISA.

Use bounded blocks for scheduling and polling. The caller owns instruction fetching, executable-page validation, block lookup, object loading, code-cache invalidation, syscall dispatch, and clearing monitors on context switches. For example, the existing `SnxIr` module/object utilities can host these functions, but its A64 function dispatcher cannot call them.

## What the backend decides

Generated code doesn't decide two things itself. It calls the runtime library, which asks the backend through `cpu->rt` (an `SnxArmRt`, `SNX_ARM_ABI` 3):

- An instruction the block compiler doesn't implement, or can't with the block's memory model, calls `snx_arm_undefined(cpu, word, reason)`. The backend runs it and returns with `exit` clear, and the block goes on after it, or it sets `exit` and the block stops at it. Without a backend, `exit` is `reason` (`SNX_ARM_UNDEFINED`, `SNX_ARM_BACKEND_UNSUPPORTED`) and `syndrome` the word, as before.
- MRC, MCR, MRRC and MCRR to coprocessors 14 and 15 call `snx_arm_coproc(cpu, word)`: which system registers user mode reaches, and what they hold, is the system's. The NX runtime gives the generic timer's CNTPCT, CNTVCT and CNTFRQ (the SDK's system tick) and the CP15 barriers. Without a backend, `snx_arm_coproc_user` gives TPIDRURW and TPIDRURO from the CPU's fields, and anything else is `SNX_ARM_UNDEFINED`.

Programs linking blocks link `snx_arm_rt`. The executor (`exec.h`, built into `snx_llvm`) compiles each block to an object and loads it with the runtime JIT's loader (`recompiler/llvm/code_loader.c`), which it gives those functions by name.

## Thumb32 instructions with A32 twins

Many Thumb32 instructions are A32 ones with their fields elsewhere: coprocessor, VFP and Advanced SIMD (the same bits, or the top byte moved), LDM/STM and PUSH.W/POP.W, the exclusives and acquire/release, barriers, MRS/MSR, long multiplies and divides, REV/RBIT/CLZ and the extends with add. `thumb_decode.c` decodes them to `TH_A32`, the A32 word and its kind, and the A32 emitter runs them reading Thumb's PC. A doubleword exclusive's second register, which Thumb names freely, comes with it. LDRD/STRD (`TH_DUAL`) are Thumb's own, as their register pair and offset are.

## Opt-in ARM11 profile and checked memory

The existing APIs still select **A57 + flat memory**; `SnxArmCPU` (`SNX_ARM_ABI=3`,
which added `rt` after the register file) and its field offsets are shared by both. No board-specific compile definition
changes shared code generation. Other boards opt in through the `_ex` APIs:

```c
SnxArmCodegenOptions options = {
    SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_CHECKED
};
snx_arm_llvm_block_ex(module, name, words, count, pc, &consumed, &options);
```

ISA and memory mode are independent. NULL options preserve the original API.
Invalid option versions/enums are refused without modifying the LLVM module.
`snx_a32_decode_profile` / `snx_thumb_decode_profile` are LLVM-independent.

The ARM11 profile implements a **conservative ARMv6K integer subset**, rejecting
MOVW/MOVT, MLS, divide, RBIT, bitfields, ARMv7 barrier encodings, acquire/release,
NEON, VFPv3/v4 and D16..D31. It recognizes the ARM11 CP15 barrier forms.
Scalar VFPv2 F32/F64 arithmetic, non-fused multiply/accumulate, compares,
conversions, register/status transfers and up-to-64-byte memory transfers are
implemented. Arithmetic uses the shared raw-bit integer FP primitives, not host
floating point or ARMv8 fused replacements. LEN/STRIDE and exception enables must
be zero for arithmetic. FPSCR writes outside NZCV, rounding mode, DN/FZ and
cumulative exceptions stop before mutation; short vectors, traps, legacy
FLDMX/FSTMX format words and larger transfers remain explicitly unsupported.
Checked VFP memory operations are aligned to four bytes and staged as one
RAM-only transaction before register/base writeback. This is not complete VFPv2
or a complete ARM11 ISA model; A57 checked vector operations remain unsupported.

ARM11 ALU writes to PC remain in ARM state. Its Thumb BL prefix and BL/BLX suffix
are **separately executable 16-bit instructions**, with the original signed
high offset and observable intermediate LR; they are not decoded as Thumb-2
S/J1/J2 branches. Thumb-2-looking bytes can be valid legacy instructions and
are interpreted accordingly, never with Thumb-2 semantics. ARM11 Thumb PC
register reads use PC+4; literal/ADR bases alone are word-aligned. Profile-only
support also covers Thumb REV, literal LDR, BKPT, high-register ADD-to-PC and
LDM base-in-list behavior. ARM11 paired transfers require eight-byte alignment.
Ordinary unaligned scalar RAM transfers model alignment checking disabled and
ARMv6 unaligned support enabled; CP15 alignment/legacy rotated-load modes are
not emulated. None of these changes affect A57's original flat-memory path.

Checked blocks require `&SnxArmCheckedCPU.cpu` (see `checked.h`), not a standalone
`SnxArmCPU`. This **separate ABI** supplies a context and one callback; no flat
pointer is read. Each instruction fetch checks execute permission and compares
its bytes with the static compilation input, even when its condition fails.
A fault reports `PREFETCH_ABORT`; a mismatch reports `CODE_CHANGED` before the
stale instruction executes. These exits must return to the dispatcher. There
is no automatic recompilation, block discovery or executable-module loader.

Integer and supported ARM11 VFP transfers use bounded little-endian transaction buffers. Reads complete
before register updates; stores commit before base/SP writeback. Paired and
multiword accesses are whole-range RAM transactions and carry `ACCESS_MULTI`,
which callbacks must reject on MMIO. This intentionally preflights all bytes,
not hardware partial-transfer behavior on abort. Ordinary aligned single MMIO
accesses remain one callback, not byte-split accesses. Callers serialize CPU,
permissions, mappings, callbacks and device/memory writes; concurrent code
mutation and a concurrent multicore scheduler are not supported. ARM11 checked
exclusives now use optional ABI-1 `READ|EXCLUSIVE` and `WRITE|EXCLUSIVE` RAM
transactions. The callback owns reservation metadata without modifying CPU
state. A serialized, saturating board-wide write/mapping epoch rejects aliases,
other-space/kernel writes and ABA changes. Unrelated writes may cause spurious
failure. B/H/word/double, CLREX and natural alignment have native tests. MMIO
exclusives are rejected. Legacy callbacks reject unknown operations and fail
closed. ABI layouts and existing operations are unchanged. A57 checked exclusives,
acquire/release, vectors and Thumb table branches remain explicitly unsupported.
The A57 flat path, including its existing atomic operations, is unchanged.

Asset-free native tests (LLVM SDK required; no NX/SDL dependencies):

```sh
cmake -S . -B build/arm11 -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF \
  -DSNX_BUILD_GUI=OFF -DSNX_TEST_ARM_AOT=ON
cmake --build build/arm11
ctest --test-dir build/arm11 --output-on-failure
```

The build-time generator emits verified native `.o` files at O0/O2; the witness
executables link only the CTR CPU/memory adapters, **not LLVM or MCJIT**. The bus
matrix compares checked and flat AOT across 1,059 integer encodings and 13 states
for both profiles at both optimization levels (55,068 comparisons). With
`SNX_TEST_ARM_DIFF=ON` and Python `unicorn==2.1.4`, the isolated build also runs
independent ARM11 MPCore integer comparisons and the existing A57 integer,
VFP/NEON and Thumb regression suites. ARM11 Thumb BL halves have C golden tests,
not a claimed independent differential oracle. The optional CTR pipeline
supplies process setup, offline conservative CFG discovery/chunking and native
module loading/dispatch under `src/3ds/`; see `docs/3DS.md`. The pinned external
SuperTux bootstrap retains the first-SVC CPU-only gate and continues through
real kernel initialization to ConnectToPort. Differential tests compare GPRs,
CPSR and RAM at both boundaries. The second CPU reference uses a test-scripted
HLE transcript, not independent hardware validation of kernel policy. Neither
reference is an execution fallback. Broader target coverage, full ARM11 semantics and
cache/resume still need work; none of these are shared A57 defaults.

## Implemented instructions (default A57/flat configuration)

- Word/byte LDR and STR, immediate and shifted-register offsets, positive/negative offsets, pre/post indexing, writeback and unprivileged aliases. Stores, like loads, take the PC (PC+8) as their base without writeback.
- LDRH, STRH, LDRSB, LDRSH, LDRD, STRD, including literal loads and applicable unprivileged aliases.
- LDM/STM IA, IB, DA, DB, PUSH/POP, and loads of PC. Register sources and writeback use the original base.
- LDREX/STREX byte, halfword, word and doubleword, CLREX, and ARMv8 LDA/STL and LDAEX/STLEX variants.
- DMB, DSB, ISB and PLD/PLDW/PLI prefetch hints. NOP, YIELD, WFE, WFI, SEV, SEVL and the unallocated hints (CSDB, ESB, DBG) complete as NOP.
- All sixteen ordinary data-processing opcodes, including ADC/SBC/RSC and tests/comparisons, with NZCV semantics and conditional execution.
- Immediate rotation, LSL/LSR/ASR/ROR/RRX, including register-shift amounts zero, 32, above 32 and multiples of 32.
- MOVW/MOVT, MUL/MLA/MLS, signed/unsigned long multiplies and accumulates, UMAAL, SDIV/UDIV.
- CLZ, REV/REV16/REVSH/RBIT, scalar byte/halfword extends and extend-adds, SXTB16/UXTB16 and their adds, UBFX/SBFX/BFI/BFC.
- The ARMv6 media and DSP instructions: SSAT/USAT/SSAT16/USAT16, PKHBT/PKHTB, SEL, the parallel add and subtract family (signed, unsigned, saturating and halving ADD16/ASX/SAX/SUB16/ADD8/SUB8, setting GE), QADD/QSUB/QDADD/QDSUB, the halfword multiplies SMLA<x><y>/SMUL<x><y>/SMLAW<y>/SMULW<y>/SMLAL<x><y>, the dual multiplies SMLAD/SMUAD/SMLSD/SMUSD/SMLALD/SMLSLD (with X), SMMLA/SMMUL/SMMLS (with R), USAD8/USADA8, with the sticky Q flag. CRC32 and CRC32C (B, H, W).
- B, BL, BX, BLX immediate/register, SVC and BKPT.
- MRS CPSR, MSR to the CPSR as EL0 writes it (NZCVQ, GE and E, the privileged fields ignored), SETEND, and user TPIDRURW/TPIDRURO accesses through MRC/MCR. A block reads E on entry, so SETEND and MSR to the x field end it; a big-endian E then stops the next block with `SNX_ARM_ENDIAN_UNSUPPORTED`.

## VFP and Advanced SIMD

Implemented for the Cortex-A57 A32 instruction set as ARMv8-A defines it, with exact raw-bit semantics rather
than host floating point:

- VFP scalar F32/F64: VADD, VSUB, VMUL, VDIV, VMLA, VMLS, VNMLA, VNMLS, VNMUL, VFMA, VFMS, VFNMA, VFNMS,
  VMOV immediate/register, VABS, VNEG, VSQRT, VCMP/VCMP with NaN behaviour, and FPSCR access through VMRS/VMSR
  and core register moves.
- VFP conversions and rounding: VCVT between F16/F32/F64 and signed/unsigned integers, fixed-point `#fbits`
  conversions, VRINTA/N/P/M/Z/R/X, VCVTA/N/P/M, and half-precision VCVTB/VCVTT.
- Advanced SIMD three-same, three-different, two-register, across-lane, element and structure groups: integer
  add/sub/saturating/halving, min/max/compare, multiply and long multiply, shifts (immediate and register),
  absolute/negate, permutes (VTBL/VTBX, VEXT, VTRN/VZIP/VUZP/VSWP), VDUP, and FP32 lanes with per-lane
  FPSCR semantics.
- FP approximation instructions VRSQRTE/VSQRTE/VRECPE/VRSQRTS/VRECPS and the integer urecpe/ursqrte forms,
  using ARM's estimate algorithms.
- VLDn/VSTn structure loads and stores (1 to 4 registers, single/multiple elements, all lanes, replicate,
  pre/post-indexed with writeback), VLD/VSTM/VLDR/VSTR (in A32 with a PC base too, VLDM/VSTM without writeback), and
  core-to-lane and lane-to-core moves.
- AES (AESE/AESD/AESMC/AESIMC), SHA1 and SHA256 (SHA1H/P/M/C/SU0/SU1, SHA256H/H2/SU0/SU1), and polynomial
  multiply VMULL.P8/P64, all through portable reference implementations. No AES-NI or CLMUL is used here.

FP exception flags (IOC, DZC, OFC, UFC, IXC, IDC) and FPSCR cumulative state follow ARM's definitions, including
flush-to-zero, default-NaN, and alternate-half-precision modes. Rounding modes RNE, RP, RM and RZ are honoured
per lane. Reciprocal step instructions round the product before subtracting, as ARMv8-A specifies, which is
not the same as a fused step.

Thumb16 and Thumb32 are supported (see `thumb_decode.c`, `llvm_thumb.inc`): integer data processing, loads/stores (immediate, register, pre/post-indexed, unprivileged), push/pop, LDM/STM, branches (B/Bcc/BL/BLX/BX, CBZ/CBNZ, TBB/TBH), bitfield (SBFX/UBFX/BFI/BFC), extends, MOVW/MOVT/ADDW/SUBW, MUL/MLA/MLS, wide data-processing with shifted and modified-immediate operands, and IT blocks (conditions applied per instruction against current flags, as ARMv8-A specifies). Thumb32 coprocessor (VFP/NEON) shares the A32 vector emitter.

## Limits and memory model

- ARMv8.1+ extensions that Cortex-A57 lacks (FP16
  arithmetic, dot-product, RDM) and privileged VFP state are not implemented. libgccjit is an explicit stub for
  both A32 and Thumb. Unsupported and rejected
  unpredictable encodings trap rather than using an A64 fallback.
- Little-endian Normal guest memory only. CPSR.E is rejected. Host-endian conversion is separate from the guest byte order.
- Effective addresses wrap at 32 bits before host pointer construction. Transfers outside the supplied flat mapping, or crossing its end, abort. Unaligned ordinary scalar accesses are supported. Multiple transfers, doubleword transfers, PC loads and atomics enforce their alignment constraints.
- LDM/STM and doubleword bounds are checked as a group before any transfer. This is a deterministic flat-memory fault policy, not emulation of partial transfers on an MMU abort.
- Acquire/release accesses and exclusives use LLVM sequentially consistent atomics. Barrier scopes are conservatively strengthened to a full fence. Hints may complete without waiting.
- The exclusive monitor is value-based CAS, like the existing A64 backend. It detects changed values but not ABA writes by another CPU. Local stores may clear it and STREX always clears it after an attempt. A globally invalidated architectural reservation monitor, device memory and concurrent self-modifying code need runtime integration. This is not a claim of complete multicore memory-model fidelity.
- Scalar loads/stores use LLVM integer accesses with explicit unaligned alignment, not typed C dereferences or byte-by-byte stores. LLVM arithmetic does not use signed-overflow promises. Dynamic shifts and signed division avoid LLVM poison on architectural edge cases.

## Tests

With Clang and LLVM development files installed:

```sh
cmake -S . -B build/a32 -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF
cmake --build build/a32 --target snx-test-arm-decode snx-test-arm-emit
ctest --test-dir build/a32 -R '^arm_' --output-on-failure
```

Optional differential suite, using a local Python environment:

```sh
python3 -m venv .venv
.venv/bin/pip install unicorn==2.1.4
cmake -S . -B build/a32 -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF \
  -DSNX_TEST_ARM_DIFF=ON -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
ctest --test-dir build/a32 -R 'arm_|thumb_' --output-on-failure
```

The suite verifies IR before and after optimization and executes generated objects at O0 and O2. The integer
suite covers 39 witness programs, including an inter-block BL/BX loop, bounds failures, address wrapping,
writeback, PC loads, failed predicates, monitor invalidation, TLS and deferred-backend exits, with 1,081
instruction/block encodings over 64 initial states each. The VFP/NEON suite compares all GPRs, D registers,
CPSR, FPSCR and guest memory for 1,490 encodings over 80 initial states each at O0 and O2 (119,200 exact
comparisons per level), including FP edge cases (NaNs, denormals, infinities, rounding-mode and FPSCR
combinations) and SIMD saturation/shift boundaries. The Thumb suite compares all GPRs, CPSR and guest memory
for 141 instruction/IT sequences at O0 and O2, covering Thumb16 and Thumb32 data processing, loads/stores,
branches, bitfield and IT blocks. Decode tests check both accepted and rejected encodings
against llvm-objdump on cortex-a57. Unicorn's ARMv8-capable AArch32 model is the reference, not Cortex-A57
hardware. Tests use no game assets.
