/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The optimizer the layers' SPIR-V shares (the Maxwell translator's, and any other's that calls it): spirv-opt's
   algorithms, each written again as a pass over a finished module's words in place, with tables indexed by id and
   by instruction and no object per instruction, so that a pass costs a few walks of the words where spirv-opt
   builds the module's whole representation. A layer runs its own pass first (what it knows before the module runs)
   and these after it. */
#ifndef SNX_SPIRV_OPT_H
#define SNX_SPIRV_OPT_H
#include <stddef.h>
#include <stdint.h>

/* Takes out of module (words of them: a whole module, as spv_emit_finish gives it) what nothing reads, as
   spirv-opt's dead code elimination does within functions: an instruction with no effect but its result, when no
   instruction left uses that result; a Function variable no instruction left reads, with the stores to it; and so on
   for what only those used. The names and decorations of what goes go with it. The rest of the module stays word for
   word, moved down over the gaps: returns the words left (words itself when nothing was dead).

   Of an instruction whose layout the pass does not know (an extension's), every operand counts as a use of the id of
   its number: a literal there can only keep what it happens to number. */
size_t spv_opt_dead_code(uint32_t* module, size_t words);

/* Value numbering, as spirv-opt's redundancy elimination does it: an instruction of a function that only makes its
   result from its operands (arithmetic, a conversion, a composite, an access chain, an image sample or query, a
   GLSL.std.450 instruction) goes when an equal one dominates it, and what read its result reads the other's. Equal
   is the same opcode, type, operands and decorations; the two operands of an integer or logical instruction that
   commutes may be swapped. An instruction that takes the fragment's implicit derivatives, and an OpSampledImage,
   only pair inside one block. A load pairs with an equal load: wherever that one dominates it when the pointer is
   into an Input, UniformConstant, PushConstant or Uniform variable (but a module that declares a BufferBlock), and
   otherwise only in the same block with no store, call, atomic, image write or unknown instruction between them.

   A result an instruction of unknown layout may name (any of its words) neither goes nor stands for another, nor
   does one with a decoration that has operands. A module with a 64-bit integer type and an OpSwitch is left as it is. Run
   spv_opt_dead_code after it: what only fed an instruction that went (an access chain, a constant's use) is dead
   then. Returns the words left (words itself when nothing was equal). */
size_t spv_opt_values(uint32_t* module, size_t words);
#endif
