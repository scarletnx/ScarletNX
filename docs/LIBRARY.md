# The recompiler as a library

## Index

* [Build and install](#build-and-install)
* [Use](#use)
* [Limits](#limits)

The recompiler can be used on its own, without the Switch runtime (HLE, GPU) or the
GNU ABI runtime: a program hands it ARM64 (A64) machine code in memory, gets host
code loaded into its own process, and runs that code on CPU states it owns, with its
own SVC handler. AArch32 (A32 and Thumb) code runs through the block compiler the same
way. The interface is `src/common/cpu/recompiler/snx_recompiler.h` and the library is
`libscarletnx-recompiler.a` (issue #171).

## Build and install

The library is built with the recompiler, in every configuration that has it. The
smallest one leaves out both runtimes:

```sh
cmake -S . -B build/tools -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF
cmake --build build/tools --target scarletnx-recompiler
cmake --install build/tools --prefix /opt/scarletnx
ctest --test-dir build/tools -R recompiler_library --output-on-failure
```

The install puts `lib/libscarletnx-recompiler.a`, the headers under `include/scarletnx/`
and a CMake package under `lib/cmake/ScarletNXRecompiler/` in the prefix. The library is
built in every configuration and installed in a tools-only one. A full build installs the
emulator alone unless `-DSNX_RECOMPILER_INSTALL=ON` is given. A consumer needs nothing
else from this tree:

```cmake
find_package(ScarletNXRecompiler REQUIRED)
target_link_libraries(my_emulator PRIVATE ScarletNX::recompiler)
```

The archive carries the function discovery, both backends' emitters, the linker and
the prebuilt runtime library every module gets. Its link line adds LLVM (always) and
libgccjit (when the build found it, see `cmake/gccjit.cmake`), which the package
records. The backends generate code for the machine they run on, as the `scarletnx`
command does: build the library on the machine that will use it, or one with the
same instruction set extensions.

`-DSNX_RECOMPILER_SHARED=ON` also builds `libscarletnx-recompiler.so` (a DLL on
Windows), which exports only the header's functions and is linked with
`ScarletNX::recompiler-shared`. It compiles the backends as position-independent code.

## Use

```c
#include <scarletnx/snx_recompiler.h>

static void my_svc(CPU* cpu, uint32_t imm) { /* cpu->x[] holds the arguments */ }
static void my_undef(CPU* cpu, uint64_t pc, uint32_t word) { cpu->pc = 0; }
static void my_brk(CPU* cpu, uint64_t pc, uint32_t imm) { cpu->pc = 0; }

struct snx_recompiler* r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
struct snx_recompiled* m = snx_recompile(r, 0x8000000, text, text_size);
if (!m) fprintf(stderr, "%s\n", snx_recompiler_error(r));

RtAPI rt;
snx_runtime_defaults(&rt);
rt.svc = my_svc;
rt.undef = my_undef;
rt.brk = my_brk;

CPU cpu = {0};
cpu.mem = guest_memory;   /* flat: guest address a is guest_memory[a], the code included */
cpu.rt = &rt;
cpu.thread = my_context;  /* yours, for the hooks */
cpu.x[31] = stack_top;
cpu.x[30] = 0;            /* the entry's return address: reaching it ends the run */
snx_recompiled_bind(m, &cpu);
snx_recompiled_run(m, &cpu, 0x8000000, 0);

snx_recompiled_free(m);
snx_recompiler_free(r);
```

`snx_recompiler_new` takes the backend: `SNX_RECOMPILER_FLAG_LLVM` (always built,
the recommended one) or `SNX_RECOMPILER_FLAG_GCCJIT` (libgccjit, for hosts LLVM has no
code generator for). `snx_recompiler_has` tells which the build and the host support.
The recompiler also takes the `-O` level (`snx_recompiler_set_optimization`, `2` by
default), how many code units compile at once (`snx_recompiler_set_jobs`: threads
with LLVM; with libgccjit, which compiles one at a time per process, child processes),
progress lines (`snx_recompiler_set_verbose`) and a folder to save the generated
code's text in (`snx_recompiler_set_keep_ir`).

`snx_recompile` recompiles the bytes of one buffer as they will be found at a guest
address (both multiples of 4), and loads the result: on Linux from an anonymous memory
file, elsewhere through a temporary file, removed once loaded on Unix and when the module
is freed on Windows, where a loaded DLL keeps its file. `snx_recompile_ranges` takes
several ranges (the `.text` of several modules) and hints: known function addresses,
such as the entry point or a symbol table. Functions are found by exploring calls and branches from the
start of each range and from the hints, as the game workflow does. Code the
exploration does not reach is not lost: the module keeps a copy of its code ranges, and
its JIT (`jit_service.c`, started by the first miss) compiles any address of them the
dispatch table has no function for when a CPU reaches it, which waits for the compile.
An address it can't compile (outside the ranges, or on a host without the JIT) is an
undefined instruction. `snx_recompiled_functions` reports what was recompiled, and
`snx_recompiled_fallbacks` is always 0: an instruction without an emitter fails the build.

The contract between the generated code and the caller is `cpu.h` (the `CPU` state,
versioned by `SNX_ABI`, which `snx_recompiler_abi` reports) and
`runtime/interface.h` (`RtAPI`). `snx_runtime_defaults` fills every hook but `svc`,
`undef` and `brk`, which the caller writes, and `snx_recompiled_bind` refuses a CPU
without them. Its `miss` runs the code the module a CPU is bound to compiles for a pc
it has no function for. `snx_svc_call64` is the type of the SVC hook: the generated code calls
it with `cpu->pc` past the SVC instruction, and the handler reads and writes the
registers. `snx_recompiled_bind` points a CPU at the module's dispatch table and code
range. Any number of CPUs, run by any number of threads, may be bound to one module.

Every recompiled function is a host function `F(CPU*, entry)` that returns when the
guest function returns, with `cpu->pc` its return address, or right after the `brk`
hook ran, or the `undef` hook for a word that decodes to nothing, with `cpu->pc` at that
instruction. `snx_recompiled_call` makes one such call. `snx_recompiled_run` repeats it
from `cpu->pc` until `cpu->pc` reaches the stop address: the caller puts that address in
`x30` before the entry, and has `undef` and `brk` set `cpu->pc` to it when they end the
run. One case is different: an instruction that decodes but whose encoding the helpers
reject (a reserved SIMD or system register form) calls `undef` from inside its body and
goes on with the next instruction, without touching `cpu->pc`. A handler that must stop
there records the fault for the caller to act on, or longjmps out of the generated code,
as the Switch runtime does with a fatal error. `snx_recompiled_lookup` gives the host
function for an address, for callers with a run loop of their own.

A just-in-time compiler is built from three more calls. `snx_recompile_from` compiles
only the functions at its hints and the functions they call, breadth first, until about
a given number of instructions, skipping call targets a callback says are compiled
already, and splits a function longer than 4096 instructions into pieces. Its module's
table covers just what it found. `snx_recompiled_function` gives the function a module
has for an address (NULL for any other), for the caller's own dispatch table. A caller with
its own table also gives `RtAPI.miss` its own handler, which compiles the code at that
address and runs it, as the Linux runtime (`src/gnuabi/runtime/jit.c`) does. A caller that
keeps `snx_runtime_defaults`' miss can instead name that handler with
`snx_recompiled_set_fallback`, which the module's misses then run instead of its own JIT. `snx_recompiler_set_cache` names a
folder that keeps linked modules (LLVM) by a hash of everything they are made from, and
the compiled runtime library: a module found there is loaded instead of compiled.

For A32 and Thumb, `snx_recompile32` prepares an address range whose blocks are compiled
on first use by the LLVM block compiler (`src/common/cpu/arm/`), and
`snx_recompiled32_run` runs an `SnxArmCPU` (`cpu/arm/arm.h`) from `r[15]` until an exit.
The SVC hook is an `snx_svc_call32`: it sees `r7` as the number and `r0` to `r5`
zero-extended, the console's 64-bit SVC surface, and returns non-zero to end the run.
The bytes are read from `cpu->mem`, so the code must be there. Ranges are multiples of 2
bytes (Thumb) and end at or below 4 GiB. `cpu->exit` is cleared when a run starts, so a
run after an exit starts over from `r[15]`. `cpu->rt` (an `SnxArmRt`, or NULL) decides
what the blocks leave to the system: instructions they can't run, and the coprocessor
registers (`cpu/arm/README.md`). With NULL, they stop with an exit, and the thread ID
registers come from the CPU's fields. libgccjit has no A32
backend, and a libgccjit recompiler refuses A32 code with a reason.

## Limits

- The host is x86-64 (the memory ordering of the generated code relies on it, `cpu.h`).
- Guest memory is flat (`cpu->mem + address`), with no page tables: the caller lays out
  the guest address space and keeps the code in it.
- One thread uses a given `snx_recompiler` or `snx_recompiled` object at a time (binding
  and running CPUs on a module from several threads is fine).
- A64 and A32 modules are separate things: never run an A32 block on a `CPU`, or hand an
  `SnxArmCPU` to A64 code.
- The dispatch table holds a pointer per instruction slot of the code range, so ranges
  spanning more than 1 GiB of guest addresses are refused.
- Out of memory ends the process, as it does everywhere in this recompiler (`snx_alloc`).
  Every other failure comes back as NULL or -1 with `snx_recompiler_error` set.

The test `tests/recompiler_library.c` is a complete example: hand-assembled A64 and A32
programs recompiled on each backend, with SVC, BRK and undefined-instruction hooks,
hints, and code compiled at its miss.
