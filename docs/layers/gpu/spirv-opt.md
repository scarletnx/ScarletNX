<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The shared SPIR-V optimizer

## Index

* [How a pass is written](#how-a-pass-is-written)
* [Value numbering](#value-numbering)
* [Integer folds and identities](#integer-folds-and-identities)
* [Known bits](#known-bits)
* [Dead code](#dead-code)
* [Tests](#tests)

`src/common/utilities/spirv_opt.c` holds the passes every layer's SPIR-V shares: spirv-opt's algorithms, each written
again over a finished module's words. spirv-opt itself stays out of the runtime's path: it builds the whole module's
representation for each run, and a shader is translated while a draw waits for it.

A layer translates a program, runs its own pass (what the layer knows before the program runs, which no general
optimizer can know), and then these:

```c
#include "common/utilities/spirv_opt.h"

uint32_t* module = spv_emit_finish(&emitter, &words);
words = spv_opt_values(module, words);
words = spv_opt_dead_code(module, words);
```

Dead code goes last: what only fed an instruction another pass took out is dead then.

The Switch layer does it in `vk_translate_spirv` (`src/nx/gpu/vk/pipeline.c`), for the Maxwell and the SM86
translators alike. The translators' own entry points (`shader_to_spirv`, `sm86_to_spirv`) give the module as it was
written, which is what their tests read.

## How a pass is written

A pass takes a whole module and its word count, changes it in place and returns the new count. It keeps no object per
instruction: its tables are arrays indexed by id (the module's bound says how many) and by instruction, in one block
that it frees before it returns. It walks the words a fixed number of times. What it does not know, it leaves: an
instruction of an extension is kept, with everything its operands might name.

## Value numbering

`spv_opt_values` takes out an instruction that computes what an instruction dominating it already computed, and
makes everything that read its result read the other's (spirv-opt's local and global redundancy elimination). Two
instructions are equal when their opcode, type, operands and decorations are; the two operands of an integer or
logical instruction that commutes (`OpIAdd`, `OpIMul`, the bitwise ones, `OpIEqual`, `OpINotEqual`, the logical
ones) may be in either order. Float arithmetic is equal only as written.

What it numbers:

- an instruction that only makes its result from its operands: arithmetic, a conversion, a composite, an access
  chain, a GLSL.std.450 instruction but Modf, Frexp and the interpolations, an image sample, fetch, gather or query;
- within one block only, an instruction that takes the fragment's implicit derivatives (the implicit-LOD samples,
  `OpImageQueryLod`, the derivatives), since a block under a branch does not have the derivatives of the block that
  dominates it, and an `OpSampledImage`, which only its own block may use;
- a load that is not volatile: of an Input, UniformConstant, PushConstant or Uniform variable (or a chain of one)
  from any block that dominates, since no instruction writes those; of any other memory within one block, up to the
  next store, call, atomic, image write or instruction it does not know. A module that declares a `BufferBlock` may
  write under Uniform, so its Uniform loads follow the second rule.

It finds each function's dominators (Cooper, Harvey and Kennedy) and then walks the instructions once, in the
module's order, where a block comes after the blocks that dominate it: each instruction's operands are replaced as it
is reached, then it is looked up in one table of the instructions kept so far, where an entry counts when its block
dominates this one. A phi that read a replaced result around a loop is fixed at the end.

What it leaves alone: an instruction of an opcode it has no layout for, with every result such an instruction's words
might name; a result with a decoration that has operands; `OpUndef`, `OpPhi`, `OpImageRead` and
`OpImageTexelPointer`; and a whole module that has both a 64-bit integer type and an `OpSwitch`, whose cases would be
three words each.

On the Maxwell translator's modules it removes the access chains, bitcasts, negations and field extracts a block
repeats, and the second load of a constant buffer word or an input.

## Integer folds and identities

The same walk folds an instruction before it looks it up, so what it folds to is what the next ones are numbered
with (spirv-opt's constant folding and instruction simplification, for what the translators write). Only 32-bit
integers of either sign and booleans, on constants that are no specialization constants:

- on constants alone: `OpIAdd`, `OpISub`, `OpIMul`, the bitwise ones, `OpNot`, `OpSNegate`, the three shifts (by less
  than 32), `OpBitFieldUExtract` and `OpBitFieldSExtract` (a field that ends by bit 32), the integer comparisons, the
  logical instructions, and an `OpBitcast` between the two signs;
- one operand that decides: `x & 0`, `x * 0`, `x | ~0`, `x - x`, `x ^ x`, `false && x`, `true || x`;
- one operand that leaves the other: `x + 0`, `x - 0`, `x | 0`, `x ^ 0`, a shift by 0, `x * 1`, `x & ~0`, `x & x`,
  `x | x`, `true && x`, `false || x`, `x && x`, `x || x`, with the constant on either side of what commutes;
- of any type: an `OpSelect` whose condition is a constant or whose two values are one id, and an `OpBitcast` to
  the type its operand has, or of a bitcast from that type.

An instruction becomes one of its operands only when that operand has the instruction's type: a signed sum of an
unsigned value and zero stays. A constant result reads the module's constant of that type and value; when there is
none, the instruction's own id becomes that constant, declared before the first function in the room the
instruction left, so a module never grows. Float arithmetic, divisions and a decorated instruction are not folded.

On the Maxwell translator's modules: `x + 0` for a register's offset, masks and shifts of immediates, selects on a
constant condition, and bitcasts there and back.

## Known bits

The walk also keeps, for each 32-bit integer value, the bits known 0 and the bits known 1 (LLVM's known bits), two
words per id: a constant's are all known; an instruction's come from its operands':

- `&`, `|`, `^`, `~`, a select (what both values know) and a bitcast between the two signs, bit by bit;
- `OpIAdd`, `OpISub` and `OpSNegate` with the carries: a bit is known where both operands' bits and the carry into
  it are (LLVM's `computeForAddCarry`);
- `OpIMul`: the low zeros of the two operands add up, and so do their high zeros past 32;
- the three shifts by a constant amount below 32, and `OpBitFieldUExtract`, `OpBitFieldSExtract` and
  `OpBitFieldInsert` with a constant offset and count that end by bit 32;
- nothing of a load, a phi, a function's parameter, or an operand of another type.

What it folds:

- an instruction whose result has every bit known: `((x << 4) + 480 >> 2) & 3` is 0, `(x & 0xffff) >> 16` is 0;
- an integer comparison the ranges decide, each operand's least value its known 1s and its most its bits that may
  be 1 (for a signed one, in the order the sign bit flipped gives): `0 > (x & 0x7fffffff)` is false, `(x & 0xff) <
  256` true; and an operand compared with itself (`x < x` is false, `x <= x` true);
- `a & b` where every bit of `a` that may be 1 is known 1 in `b`, `a | b` where every bit of `b` that may be 1 is
  known 1 in `a`, and `OpBitFieldUExtract` from bit 0 that keeps every bit of its base that may be 1 (signed, one
  whose base's bits from its sign bit up are all known alike): each is `a`.

A boolean is known only as a constant, which the folds above take already. The comparisons then fold the selects
and logical instructions on them as constants.

On the Maxwell translator's modules it removes the component of an indexed constant buffer read: the translator
splits an address made of a register shifted by 4 plus an offset into a vec4's index and `(address >> 2) & 3`, and
the low bits of such an address are known. Also the sign tests of masked values (an absolute value or a clamp of what
cannot be negative) and the fields past a value's width.

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
branch, or anything outside a function (a type, a constant or a global variable nothing uses stays). Values that only
feed each other around a loop (a phi and its own increment) stay too.

On the Maxwell translator's modules it removes the registers, predicates and flags a block stores for later blocks
that never read them.

## Tests

- `spirv_opt` (`tests/spirv_opt.c`), `spirv_opt_values` (`tests/spirv_opt_values.c`) and `spirv_opt_fold`
  (`tests/spirv_opt_fold.c`): each rule above on a module written word by word. `spirv_opt_fold` also makes 4000
  modules at random (a fixed sequence) of values with a few unknown bits, runs each before and after the pass on
  every value of those bits, and checks they store the same.
- `vulkan_headless`, `check_translation_dead_code` and `check_translation_values`: the runtime's module is the
  translator's with the passes run, and the device takes it.
