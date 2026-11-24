---
name: snx-manual
description: "The manuals of the ARM instruction sets, the Maxwell and Ampere GPUs and the NVN API that ScarletNX publishes. Read the page of an encoding, a rounding rule or an API contract before you write it. Use it when you write a decoder, a Sail file, an emitter, a translator or an NVN handler. Use it also for a test that checks one of them."
---

# The published manuals

`scarletnx.github.io/manual` holds the specifications that this project recompiles against. One page shows one
instruction or one API function. An instruction's page gives its encodings, its decode clause and its operation. A
function's page gives its signature, the consoles that have it and the handler that runs it. Read the page before you
write the code. Do not take an encoding or a rule from the output of the runtime.

| Area | Manual | The code that it governs |
| --- | --- | --- |
| A64 | https://scarletnx.github.io/manual/a64/ | `src/common/cpu/aarch64/sail/a64.sail` (sail-arm's text), `src/common/cpu/aarch64/sail/nx.sail` (the fixups), `src/common/cpu/aarch64/decode/a64.decode`, `src/common/cpu/aarch64/decode/a64.profile` |
| A32 and T32 | https://scarletnx.github.io/manual/a32/ | `src/common/cpu/arm/sail/a32.sail` and `src/common/cpu/arm/sail/arm.sail`, `src/common/cpu/arm/decode/` |
| Maxwell, SM53 | https://scarletnx.github.io/manual/sm53/ | `src/nx/gpu/shader/sail/sm53_*.sail` and `src/nx/gpu/shader/sail/gpu.sail`, `src/nx/gpu/shader/sm53.decode` |
| Ampere, SM86 | https://scarletnx.github.io/manual/sm86/ | `src/nx/gpu/shader/sail/sm86*.sail`, `src/nx/gpu/shader/sm86.decode` |
| NVN and NVN2 | https://scarletnx.github.io/manual/nvn/ | `src/nx/gpu/nvn/nvn.hook`, its handlers, `nvn.h` |

The Sail holds the operation of an instruction. The build makes `a64_sail.inc`, `a32_sail.inc` or `<isa>_sail.inc` in
the build folder from it, and `ir_sail.c` or the translator includes that file. Change the Sail, and not the generated
file. An emitter that no Sail makes (`ir_fpu.c`, `ir_vec.c`, the shader back ends) still takes its rule from the page.
`docs/layers/cpu/guide.md` says which emitter holds which operation.

The manuals cover the instruction sets and NVN, and they are the first reference for both. Use another reference only
for what they do not cover. A Switch service has no page here, and `CLAUDE.md` names the reference for it.

## How to read a page

- The search takes a name or an instruction word. A name finds the nearest names. An A64 word is `0x1a020020`, a
  Thumb word is one halfword, and a GPU word is 64 bits, whose opcode is bits 63:48. A page is
  `.../a64/<encoding>.html` for an encoding and `.../nvn/f/<function>.html` for a function, so a citation names it.
- The site's workflow makes the pages from ScarletNX's `main` each day. Thus a page can be newer than your branch, and
  older than a Sail change of yours that is not on `main` yet. Read the Sail file in the tree when they differ, because
  a page only renders it. To render the pages for your own tree, which is rare, clone
  `scarletnx/scarletnx.github.io` with `--depth 1` and run `python3 tools/gen-manual.py <checkout>` there. Quote the
  commit in the footer when a claim rests on the page.
- The pages hold the instructions that the area's `.roots` file names (`a64.roots`, `a32.roots`,
  `sail/sm53.roots`, `sail/sm86.roots`). An instruction that has no page is one that no root reaches. Add its decode
  function to the `.roots` file with the rest of the change.
- The Maxwell pages have 14 of 154 instructions without Sail, only an encoding, and the `Without Sail` page lists
  them. The Ampere pages have 42 of 178. For one of them, read the encoding, then write the operation from other
  evidence and say what that evidence was.
- An NVN page names the line of `nvn.hook`, the consoles that have the function and the handler with its file. Read
  the consoles' marks before you change a handler that both of them use. `missing.html` lists the driver's functions
  that nothing implements, so a new handler makes it shorter. The formats' Vulkan equivalents come from
  `src/nx/gpu/vk/format.c`.

## Rules

- Name the manual page in the commit message or the report when a claim rests on it.
- An expected value in a test comes from the manual. Name the page in a comment on the case, for example
  `// <manual>, <page>, <encoding name>`.
- Do not take an expected value from the current output of the runtime. That proves only that the output did not
  change.
- qemu-aarch64 is a second oracle for the CPU emitters. Check the case against the manual too.
- When the manual and a game disagree, write down both. The game shows what the hardware does, and the manual gives the
  specification.
- When a test fails, decide if the runtime or the manual is wrong. Do not change the expected value to make the test
  pass.
- Do not copy a table or a chapter of a manual into a file in this repository. Link the page and quote the one line that
  decides the question.
- Name a repository as `scarletnx/ScarletNX` or `scarletnx/scarletnx.github.io`, never with a personal owner. Write a
  local tree as the checkout in hand, never as the path of one machine.

## Tests

- `ctest --test-dir build -R sailgen`: the generator against its fixtures (`sailgen_a64`, `sailgen_a32`,
  `sailgen_extract`, `sailgen_sdm`, the `isel` cases). Run it after a change to a Sail file.
- `ctest --test-dir build -R llvm_qemu_differential`: the LLVM emitters against qemu-aarch64. The mismatches that
  remain are in `tests/llvm_qemu_expected.txt`. The pattern also takes `_jit`, `_avx2`, `_abi`, `_portable` and
  `_generic`. `_sweep` needs the `SNX_TEST_LLVM_SWEEP` configure option.
- `arm_decode` and `arm_decode_tree`: the A32 and Thumb decoders.
- `a64_decode`, `a64_decode_group`, `a64_fp_witnesses` and `a64_fp_arithmetic_witnesses`: the A64 decoder and its
  floating point rules.
- `shader_decode`, `shader_structure`, `shader_program`, `shader_sm86` and `shader_sail_sm86`: the shader decoder and
  translator.
- `nvn_header`, `nvn_descriptors`, `nvn_tex_storage`, `nvn_commands`, `nvn_names` and `nvn_contracts`: the NVN
  handlers.
