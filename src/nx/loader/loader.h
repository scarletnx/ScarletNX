/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The recompiler's view of a Switch program: its NSO or NRO modules decoded and laid out at the guest addresses the
   runtime gives them, with what function discovery starts from (symbols, relocated pointers, unwind ranges) and the
   program's metadata (NPDM, control.nacp, RomFS) */
#ifndef SNX_TOOL_LOADER_H
#define SNX_TOOL_LOADER_H

#include "common/utilities/common.h"
#include "common/cpu/recompiler/image.h"
#include <stddef.h>

/* Where the first module goes: a 64-bit process's code region, and a 32-bit one's (mem.h's AS32_CODE_BASE). Each
   further module follows the previous one after a guard page */
#define SNX_CODE_BASE   0x08000000UL
#define SNX_CODE_BASE32 0x00200000UL
#define SNX_PAGE_SIZE   0x1000UL

/* A defined dynamic symbol. value is module-relative; type is the ELF STT_ value */
typedef struct {
    SnxU64 value, size;
    char* name;
    unsigned type;
} SnxSymbol;

/* MOD0 and what its fields point at, as module offsets */
typedef struct {
    SnxU64 off, dynamic, bss_start, bss_end, ehf_start, ehf_end;
} SnxMod0;

/* One decoded module. Every offset in it is from the module's start, and base is where it sits in the guest */
typedef struct {
    char *name, *path;
    unsigned char* image; /* text, rodata and data as they are mapped, without the BSS */
    size_t image_size;
    SnxU32 text_off, text_size, ro_off, ro_size, data_off, data_size, bss;
    unsigned char build_id[32];
    SnxU64 base, size; /* size: the whole mapping, BSS included, page-rounded */
    SnxSymbol* symbols;
    size_t symbol_count;
    SnxU64 *functions, *relptrs; /* discovery seeds, both sorted without duplicates */
    size_t function_count, relptr_count;
    SnxU64* exported; /* the functions its dynamic symbols define, sorted without duplicates (in the functions' block) */
    size_t exported_count;
    SnxFde* fde;
    size_t fde_count;
    char** imports; /* undefined symbols' names */
    size_t import_count;
    int is_nro, has_mod0;
    SnxMod0 mod0;
    char* eh_error; /* why the unwind table was read only partly, or NULL */
    /* An NRO's assets (ASET): its control.nacp, owned by the module, and where its RomFS lies in the file, which is
       never read (it can take gigabytes) */
    unsigned char* nacp_data;
    size_t nacp_size;
    SnxU64 romfs_offset, romfs_size;
} SnxModuleImage;

/* The NPDM fields the recompiler and the runtime use */
typedef struct {
    SnxU64 title_id;
    SnxU32 stack;
    unsigned prio, core, aspace;
    int is64;
    char name[17];
} SnxNpdm;

typedef struct {
    SnxModuleImage* modules; /* in load order (rtld, main, subsdk0..9, sdk), the ones present */
    size_t module_count;
    unsigned char* npdm_data; /* main.npdm as read, NULL when the program has none */
    size_t npdm_size;
    SnxNpdm npdm;
    char *entry_path, *exefs_path, *root_path, *romfs_path;
    /* An NRO's embedded RomFS: where it lies in entry_path, romfs_size 0 when it has none */
    SnxU64 romfs_offset, romfs_size;
    /* The program's control.nacp, owned by the program, or NULL: an NRO's from its assets, an ExeFS's from beside it
       (<root>/control.nacp, written there by the extraction) */
    unsigned char* nacp;
    size_t nacp_size;
} SnxProgram;

/* The loaders return 0, or -1 with a message in error (cut to error_size, always terminated) and the output struct
   left zeroed. Functions, relptrs, FDEs and MOD0 are module offsets. A broken unwind table keeps the FDEs read before
   the damage and says why in eh_error; any other malformed structure refuses the module.

   snx_load_program: entry is an extracted program (a folder holding exefs/main, ExeFS/main or main) or an .nro file */
int snx_load_program(const char* entry, SnxProgram* program, char* error, size_t error_size);
/* An ExeFS read other than from a folder (a dump's, read in place): read gives the bytes of its file name in *data
   (malloc'd) and *size, and 0, else 1 when it has no such file, -1 when it cannot be read */
typedef struct {
    int (*read)(void* source, const char* name, unsigned char** data, size_t* size);
    void* source;
} SnxExefsReader;
/* snx_load_program for the program of exefs: entry, the dump, is its entry_path and names it in messages, and nacp
   (NULL for none) becomes its control.nacp, taken by the program even when it fails. No exefs_path, root_path or
   romfs_path */
int snx_load_program_from(const SnxExefsReader* exefs, const char* entry, unsigned char* nacp, size_t nacp_size,
                          SnxProgram* program, char* error, size_t error_size);
/* exefs with mods' files over it (common/utilities/mods.h): its file names[i] read from paths[i] instead, count of
   them, any other from under. reader reads it; the arrays are the caller's, kept while it is read */
typedef struct {
    SnxExefsReader reader;
    const SnxExefsReader* under;
    char* const* names;
    char* const* paths;
    size_t count;
} SnxExefsMods;
void snx_exefs_mods(SnxExefsMods* mods, const SnxExefsReader* under, char* const* names, char* const* paths,
                    size_t count);
/* snx_load_program with the ExeFS of an extracted program read through snx_exefs_mods: names[i] from paths[i]. An NRO
   takes none */
int snx_load_program_mods(const char* entry, char* const* names, char* const* paths, size_t count, SnxProgram* program,
                          char* error, size_t error_size);
void snx_free_program(SnxProgram* program);
int snx_load_nso(const char* path, const char* name, SnxModuleImage* module, char* error, size_t error_size);
/* An NSO of a 32-bit program: an ELF32 dynamic table, 16-byte symbols, REL and RELA relocations. Code addresses among
   its functions and relptrs keep bit 0 when it marks Thumb code */
int snx_load_nso32(const char* path, const char* name, SnxModuleImage* module, char* error, size_t error_size);
int snx_load_nro(const char* path, SnxModuleImage* module, char* error, size_t error_size);
void snx_free_module(SnxModuleImage* module);
/* data NULL with size 0 gives the defaults of a program without an NPDM */
int snx_parse_npdm(const unsigned char* data, size_t size, SnxNpdm* npdm, char* error, size_t error_size);
/* The NPDM snx_load_program would use for entry, without loading any module: an NRO's (the defaults), the ExeFS's
   main.npdm, or the defaults when it has none */
int snx_load_npdm(const char* entry, SnxNpdm* npdm, char* error, size_t error_size);
/* Loads entry as snx_load_program does and writes each module into outdir (created if missing), for tools that read
   game code at its run-time addresses (tools/symbols.py, tools/disasm.py): <name>.bin, the module's image, and
   modules.txt, a text index of lines with space-separated fields, numbers in hex without 0x:

       program 1 <is64: 1 or 0>
       module <name> <base> <size> <image size>
       symbol <value> <size> <type> <name>

   Each symbol line belongs to the module line before it; its value is module-relative and the name runs to the end
   of the line. 0, or -1 with a message */
int snx_dump_program(const char* entry, const char* outdir, char* error, size_t error_size);

#endif
