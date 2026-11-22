<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Constant buffer words folded into a translation

## Index

* [Which words](#which-words)
* [How many translations](#how-many-translations)
* [Made beside the draws](#made-beside-the-draws)
* [What it costs](#what-it-costs)
* [Where](#where)

A fragment program loads most of its constant buffer words at constant places: the bank and the offset are in the
instruction. Its draws mostly find the same words there (a material's switches, a light count, a zero that turns a
term off), and a driver that knew them would drop the multiply, the add or the branch they feed. So a translation
made for a draw holds the words that are worth it as constants, and serves the draws whose buffers hold the same
words. It is the Switch layer's own pass over a program, what the layer knows before the program runs, beside the
passes every layer shares ([spirv-opt.md](spirv-opt.md)).

The numbers below are one game's programs (every distinct one in its shader files) as RADV compiles them on an RX
6700 XT with Mesa 26.2.6: the fragment or vertex executable's instruction count from
`VK_KHR_pipeline_executable_properties`, read by `snx-shaderperf`. Nothing in them is timed but the cost of a draw.

## Which words

`shader_cb_fold_value` (`src/nx/gpu/shader/shader.h`): zero, 1.0f and a whole number up to 255, whatever the bank.
No bank is named anywhere: the banks that gain are the ones that hold such words.

| What a translation holds as constants | Instructions of 465 fragment programs | Programs with fewer, with more |
| --- | --- | --- |
| nothing | 219,505 | |
| the words of those values that the program's first draw of a scene found | 171,570 (-21.8%) | 442, 0 |
| of them, the ones the scene never changed | 172,429 (-21.4%) | 442, 0 |
| every word the scene never changed, whatever its value | 163,351 (-25.6%) | 455, 6 |

The words of the three values are a third of the loads a scene never changes and bring 85% of what all of them
would. Any other value is an immediate of its own where one vector load brought four words, and a colour or a matrix
that changes is another translation each time.

Vertex programs are not folded. The same rule takes 4.1% of the instructions of the game's 457 vertex programs
(184,935 to 177,277) and leaves 105 of them longer, for as many more translations and checks as the fragment
programs cost.

## How many translations

A program's translations differ in the words they hold, and a draw takes the one that holds most of those whose words
its buffers still have (`sh_holder`, `src/nx/gpu/vk/pipeline.c`). The first, made when none holds, folds none
(`VK_CB_FOLD_LOADS`): it holds for every draw, and records each load. The first draw of each submit that takes a
translation counts the words one made now would fold where it left loads (`vk_cb_fold_words`); after
`SH_CB_FOLD_STABLE`, 4, submits in a row that found the same words (`vk_cb_stable_note`), that one is made, by
`vk_cb_fold_mode` of how many folds the program has:

1. The first `SH_CB_FOLD_VARIANTS`, 2, fold by value: every word the draw holds that `shader_cb_fold_value` takes.
2. The next folds only the words those agree on: a word one of them left a load, or folded to another value, stays a
   load. It serves a program whose words change from draw to draw.
3. After it, none folds.

A translation with nothing more to fold is asked again only when another of its program stops holding: the words
changed. Words that change from submit to submit are never folded, and their draws go on with the first translation.

A scene of the game played for 2,000 presents from an empty shader file, with each fold made at the first draw that had
its words (before the count of submits):

| By value | Translations | Pipeline parts | Folded words a draw runs with | Draws with folded words |
| --- | --- | --- | --- | --- |
| no fold | 1,001 | 941 | | |
| 1 | 1,496 | 1,416 | 3.18 | 78.2% |
| 2 | 1,544 | 1,430 | 3.41 | 84.2% |
| 3 | 1,601 | 1,464 | 3.53 | 86.1% |
| 4 | 1,658 | 1,518 | 3.58 | 86.3% |
| 8 | 1,747 | 1,691 | 3.59 | 84.7% |

The second by value adds 3.2% to the translations and 7.2% to the folded words; the third adds 3.7% and 3.5%, and
each one after it less for more.

## Made beside the draws

A fold is made on the pipeline workers (`vk_shader_fold_draw`, `sh_fold_queue`), after the draws' own pipelines and
before the shader file's, and the draws go on with the translation they had. The worker reads no guest memory: the
draw gives it the program and copies of the words its translation read and of those at its loads, so the fold holds
the words the draws had then. Once it is made, the next draws of the translation they had make its pipeline with their
keys on the workers too (its twins, behind the shader file's pipelines), and it goes in the tables a submit after the
last of them was queued, once all are made: the draw that takes it finds its pipeline made and waits for no compile. A
state of the program first drawn after that makes its pipeline as any new state does.

A fold serves only a draw whose words it holds: each draw checks its folded words against its buffers, as it does for
any translation. A word that changed after the fold was made, before it went in the tables or after, makes the draw
take the translation that loads it, so no draw is drawn with an old word. A fold whose words no draw had in
`SH_CB_FOLD_STABLE` submits is dropped, and one with a pipeline the device could not make is never put in the tables.

## What it costs

- **A draw checks its fragment shader's folds.** `vk_cb_folds_hold` (`src/nx/gpu/vk/cb_fold.c`) reads a bank's
  folded words from one range of its bytes and compares them, 3.4 words a draw. The GPU thread's CPU time a draw goes
  from 4.15 to 4.33 us (+4.3%; -O2, the shader file warm, 8 million draws of the scene). A check's answer kept for
  the draws of a submit that bind the same buffers answers 79% of the checks and measures the same, 4.27 us against
  4.09, so each draw checks.
- **A scene's first run translates and compiles more**, the table's 54% and 52% at 2, and a program whose words stay
  once more, for the translation that folds none. The folds are translated and their pipelines made on the workers.
  A fold being translated holds the translator's lock, which a draw's own translation then waits for. The
  translations are kept in the shader file with the words they hold, as a translation that read a BRX's jump table
  is, so a later run finds them.
- **Words the GPU holds are not read.** A buffer with a result kept on the GPU (`memory/own.c`) or pending over it is
  read by a draw on the GPU; reading it here would land it. `gmem_cpu_bytes` gives no bytes there: the word stays a
  load, and a translation that folded it does not hold.

## Where

| What | Where |
| --- | --- |
| The rule and the cap | `shader_cb_fold_value`, `SH_CB_FOLD_VARIANTS` in `src/nx/gpu/shader/shader.h` |
| The translator's question at a load | `cbuf_entry` in `src/nx/gpu/shader/emit_memory.c`, `g_shader_cb_fold` |
| What a translation records, when it holds, which is made next | `src/nx/gpu/vk/cb_fold.c`, with no GPU |
| The lookup | `vk_shader_get_ex` in `src/nx/gpu/vk/pipeline.c`: `sh_holder`, `sh_fold_plan` |
| The draws' count and the folds made beside them | `vk_shader_fold_draw` in `src/nx/gpu/vk/pipeline.c` (`ShFold`), called by `render.c` |
| Tests | `vk_cb_fold` (the unit), `shader_direct_regressions` (the translator), `vulkan_headless` (draws and lookups) |

The log's periodic line of shaders translated is followed by how many of them were translated again on the workers to
hold their draws' words, the pipelines made for them before a draw took one, and those dropped.
