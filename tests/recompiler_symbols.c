/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* recompiler_symbols' program (tests/recompiler_symbols.cmake): nothing of its own. Its link takes every object of the
   recompiler's archive and LLVM and the system's libraries alone, so it links only when those define every symbol the
   archive calls */
int main(void) { return 0; }
