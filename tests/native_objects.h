/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The objects elf_relocate.c and coff_arm64.c link and load, made with the native back ends' object builder (x64_elf.h,
   in native_objects.c: its SNX_ELF_ names are elf_object.h's too) and carrying the relocations those back ends write:
   AArch64's and x86-64's code written here, as their back ends lay it out (a datum's address in a slot of the object's,
   which ABS64 fills), a RISC machine's made by its back end's descriptor functions (native.h). Each returns 0, or -1
   with what failed in error */
#ifndef SNX_TESTS_NATIVE_OBJECTS_H
#define SNX_TESTS_NATIVE_OBJECTS_H
#include <stddef.h>

/* the ELF machines the functions below take, <elf.h>'s EM_X86_64 and EM_AARCH64 */
enum { NOBJ_X86_64 = 62, NOBJ_AARCH64 = 183 };

/* AArch64, ELF or (coff) COFF: caller() = callee(*data + 1) + 1, with a frame as arm64_codegen.c's prologue has it
   (so COFF's has its .pdata): data's address in a slot of the object's (ABS64; ELF's .data.rel.ro, COFF's .rdata)
   read by ADRP and LDR (ADR_PREL_PG_HI21, LDST64_ABS_LO12_NC), callee called by BL (CALL26) */
int nobj_a64_caller(int coff, const char* callee, const char* data, const char* caller, unsigned char** out,
                    size_t* size, char* error, size_t error_size);
/* machine's (EM_AARCH64 or EM_X86_64), ELF or COFF: ext_data = 41 and ext_fn(x) = 3x */
int nobj_definitions(unsigned machine, int coff, unsigned char** out, size_t* size, char* error, size_t error_size);
/* bytes of code, all zero, which make a load or a module as large as they say */
int nobj_filler(unsigned machine, int coff, size_t bytes, unsigned char** out, size_t* size, char* error,
                size_t error_size);
/* machine's (EM_AARCH64 or EM_X86_64) need0 and need1: need<i>(x) = call<i>(x + *data<i>) + 1, the data's addresses in
   slots of the object's (one a name), read by ADRP and LDR, or a RIP-relative MOV (PC32); each call AArch64's BL
   (CALL26) or x86-64's CALL (PLT32) */
int nobj_need(unsigned machine, const char* call1, const char* call2, const char* data1, const char* data2,
              unsigned char** out, size_t* size, char* error, size_t error_size);
/* machine's data alone: other_data = 7 (.data), held_table, 64 constants (.rodata), held_zeros, 4 KiB (.bss) */
int nobj_held(unsigned machine, unsigned char** out, size_t* size, char* error, size_t error_size);

/* a code relocation of a RISC machine's object: in function fn, at bytes past its start, of sym plus addend */
typedef struct {
    char fn[16];
    size_t at;
    char sym[16];
    long long addend;
    unsigned type;
} NobjSite;
/* the objects of the RISC machine whose native back end is named desc (native.h's "riscv64", "ppc64le"...), with its
   ELF machine, e_flags and byte order: kind 0 the caller's (caller returns &ext_data, tail_direct a tail call of
   ext_fn, tail_pointer one through ext_pointer, third returns &ext_table, and caller_slot, a slot holding ext_data's
   address), 1 the definitions' (ext_data = 41, ext_fn(x) = 3x, which runs where the machine is MIPS64's or s390x's,
   and ext_pointer = &ext_fn), 2 the data's alone (ext_table = {5, 6, 7}). sites gets each code relocation, up to max;
   a descriptor this build has none of fails, said */
int nobj_desc(const char* desc, int kind, NobjSite* sites, size_t max, size_t* nsites, unsigned* em, unsigned* flags,
              int* big, unsigned char** out, size_t* size, char* error, size_t error_size);

#endif
