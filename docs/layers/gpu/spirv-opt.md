<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The shared SPIR-V optimizer

`src/common/utilities/spirv_opt.c` holds the passes every layer's SPIR-V shares: spirv-opt's algorithms, each written
again over a finished module's words. spirv-opt itself stays out of the runtime's path: it builds the whole module's
representation for each run, and a shader is translated while a draw waits for it.

A layer translates a program, runs its own pass (what the layer knows before the program runs, which no general
optimizer can know), and then these:

```c
#include "common/utilities/spirv_opt.h"

uint32_t* module = spv_emit_finish(&emitter, &words);
words = spv_opt_dead_code(module, words);
```

The Switch layer does it in `vk_translate_spirv` (`src/nx/gpu/vk/pipeline.c`), for the Maxwell and the SM86
translators alike. The translators' own entry points (`shader_to_spirv`, `sm86_to_spirv`) give the module as it was
written, which is what their tests read.

## How a pass is written

A pass takes a whole module and its word count, changes it in place and returns the new count. It keeps no object per
instruction: its tables are arrays indexed by id (the module's bound says how many) and by instruction, in one block
that it frees before it returns. It walks the words a fixed number of times. What it does not know, it leaves: an
instruction of an extension is kept, with everything its operands might name.

## Dead code

`spv_opt_dead_code` takes out of a module's functions:

- an instruction with no effect but its result (arithmetic, a conversion, a composite, a load, an access chain, an
  image sample or query, a GLSL.std.450 instruction but Modf and Frexp, a phi), when no instruction left uses the
  result;
- a Function variable no instruction left reads, with the stores to it;
- what only those used, and their names and decorations.

It walks the functions backwards, so an instruction's operands are looked at after the instruction went. A variable
whose last read goes after the stores that follow that read were passed takes one more walk, 16 at most.

It never takes out a store to anything but such a variable, a call, an atomic, an image write, a volatile load, a
branch, or anything outside a function (a type, a constant or a global variable nothing uses stays).

On the Maxwell translator's modules it removes the registers, predicates and flags a block stores for later blocks
that never read them.

## Tests

- `spirv_opt` (`tests/spirv_opt.c`): each rule above on a module written word by word.
- `vulkan_headless`, `check_translation_dead_code`: the runtime's module is the translator's with the pass run, and
  the device takes it.
