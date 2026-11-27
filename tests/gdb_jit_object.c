/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gdb_jit.c's object, made with the native back ends' ELF builder (x64_elf.h) */
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include <stdint.h>
#include <stdio.h>

/* snx_test_fn, four bytes of code (never run), and snx_test_value, an 8-byte global holding snx_test_fn's address: code,
   data and a relocation between them, in an object of the host's native back end (machine 0) or of machine's, whose
   64-bit absolute relocation is pointer */
int gdb_jit_object(const char* const names[2], unsigned machine, unsigned pointer, unsigned char** data, size_t* size,
                   char* error, size_t error_size);
int gdb_jit_object(const char* const names[2], unsigned machine, unsigned pointer, unsigned char** data, size_t* size,
                   char* error, size_t error_size) {
    static const unsigned char code[4] = {0x90, 0x90, 0x90, 0xc3};
    const uint64_t zero = 0;
    SnxXElf* o = machine ? snx_xelf_new() : snx_native_data(&pointer);
    if (!o) {
        snprintf(error, error_size, "no object of the %s back end", snx_native_name());
        return -1;
    }
    if (machine) snx_xelf_set_machine(o, machine);
    const size_t fn = snx_xelf_put(o, SNX_ELF_TEXT, code, sizeof code, 16);
    snx_xelf_symbol(o, names[0], SNX_ELF_TEXT, fn, sizeof code, SNX_ELF_GLOBAL | SNX_ELF_FUNC);
    const size_t value = snx_xelf_put(o, SNX_ELF_DATA, &zero, sizeof zero, 8);
    snx_xelf_symbol(o, names[1], SNX_ELF_DATA, value, sizeof zero, SNX_ELF_GLOBAL);
    snx_xelf_reloc(o, SNX_ELF_DATA, value, names[0], pointer, 0);
    const int rc = snx_xelf_write(o, data, size);
    snx_xelf_free(o);
    if (rc) snprintf(error, error_size, "the object: %s", rc == -1 ? "out of memory" : "no form of it");
    return rc ? -1 : 0;
}
