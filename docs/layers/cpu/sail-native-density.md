<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Sail native back end instruction density

This page records how to count the host instructions emitted for the Sail-generated x86-64 back end. It keeps two counts apart because they answer different questions.

## Path one: the generated Sail step

`snx-test-sail-x64 --counts FILE` emits the hex bytes of the Sail step for each guest word. A driver disassembles those bytes and counts instructions.

```sh
build/tests/snx-test-sail-x64 --counts words.txt
```

Use `llvm-objdump` or capstone to count the instructions in each byte string. Report the word count, the total, the mean and the host features. The baseline on this path has 19,609 decoder words, all made, 1,448,483 host instructions and a mean of 73.87 host instructions per guest word.

This path calls the generated step directly. It overestimates the groups that `src/common/cpu/recompiler/x64/x64_vec.c` intercepts on the real codegen path. Treat it as an upper bound for generated code density.

## Path two: the real codegen path

`snx-test-recompiler-backends --counts FILE` goes through the recompiler's own interface and writes the native object's bytes for each target op's words with RETs. Each record is an `OP` line with the op name and the size, followed by a line of hex bytes. `--counts-blocks N FILE` puts N guest words in each function. The function shares the frame cost across the words. `--coverage` writes one line per op. The line gives the words the back end makes with its own instructions, with a helper, and leaves to LLVM.

```sh
build/tests/snx-test-recompiler-backends --coverage > coverage.txt
build/tests/snx-test-recompiler-backends --counts objects.txt
build/tests/snx-test-recompiler-backends --counts-blocks 16 objects16.txt
```

Write each record's bytes to a file and count instructions with `llvm-objdump -d --no-show-raw-insn`. Pair the count with the op's guest words from `--coverage`. The checked-in `tests/x64_density.py` does this against limits in `tests/x64_density_targets.txt` and runs as the `recompiler_backends_density` ctest.

The baseline on this path has 7,776 switch target words and 586,859 host instructions in the native objects. The mean is 75.47 host instructions per guest word. The back end makes 6,370 words with its own instructions and 1,406 with an exact helper. It leaves no words to LLVM.
## Result

A change to the back end must report its totals and means beside the baseline. Report the word count, the path, the target and both means. Compare the same word pool and the same method. The density ctest holds each op to a limit in `tests/x64_density_targets.txt`.

A separate count of x86 LLVM code gives 80,226 instructions over 2,640 words, a mean of 30.4 host instructions per guest word. That is a different word pool, so use it only as a rough yardstick beside the two paths above.

## Use of the baseline

Report the host features, the path, the target, the word count, the total and the mean. Keep the command and the method the same when you repeat the measurement. A lower mean on one path has not by itself improved game speed. Use the profile of a real scene and its timings for a speed claim, as the CPU guide asks.
