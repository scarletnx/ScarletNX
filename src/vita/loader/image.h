/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_IMAGE_H
#define SNX_VITA_IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The game's executable (eboot.bin) as the runtime loads it and the compiler compiles it: a SELF with plain segments
   (homebrew's fake one, or a retail game's decrypted one) or the ELF itself. An ET_SCE_RELEXEC module is placed at
   VITA_LOAD_BASE with its SCE relocations applied, an ET_SCE_EXEC module (a retail game's) at its own addresses. Each
   function it imports has a stub in its text, which the module loader fills on the console; here the stub becomes
   "svc #index; bx lr" (A32), with the index local to its module. The runtime identifies the import by the stub's
   guest address. Libraries from sce_module/ are loaded beside the main image, and their exports are called as guest
   functions. Each imported variable gets a slot of VITA_VARIABLE_SIZE bytes from VITA_VARIABLES, and the code
   referring to it is relocated to that slot. The placement is stable, so compiled blocks hold the same guest addresses. */
#define VITA_LOAD_BASE  0x81000000u
#define VITA_IMAGE_END  0x82000000u /* where the user memory starts (vita/runtime/process.h) */
#define VITA_SEGMENTS   32u
/* The imported variables of every loaded module share one table, below VITA_LOAD_BASE and clear of the runtime's own
   page (0x80000000) and the threads' TLS (0x80010000..0x80050000). The stride is generous enough for any variable
   (a 32-bit pointer's, a small struct's), and the region runs all the way up to VITA_LOAD_BASE: VITA_VARIABLE_COUNT
   comes from that address space, not from a fixed number. */
#define VITA_VARIABLES      0x80100000u
#define VITA_VARIABLE_SIZE  0x100u
#define VITA_VARIABLE_COUNT ((VITA_LOAD_BASE - VITA_VARIABLES) / VITA_VARIABLE_SIZE) /* 0xF000: 61,440 slots */
/* the SVC immediates at and above this are the runtime's own (vita/runtime/process.h), below it an import's index */
#define VITA_SVC_RUNTIME 0x00ff0000u

typedef struct {
    uint32_t address, size; /* in the guest, size the memory size (BSS included) */
    uint32_t flags;         /* ELF's PF_X (1), PF_W (2), PF_R (4) */
    unsigned char* bytes;   /* size bytes, zero past the file's */
} VitaSegment;
typedef struct {
    uint32_t nid, library_nid, stub, svc; /* guest stub/slot and its module-local SVC immediate */
    char library[32];
} VitaImport;
typedef struct {
    uint32_t nid, library_nid, address;
    bool variable;
} VitaExport;
typedef struct {
    VitaSegment segment[VITA_SEGMENTS];
    unsigned segment_count;
    char name[28];
    /* guest addresses, a function's with its Thumb bit; 0 for none */
    uint32_t module_start, module_stop, process_param;
    VitaImport* imports;
    uint32_t import_count;
    VitaImport* variables; /* each imported variable once, its slot in stub */
    uint32_t variable_count;
    VitaExport* exports;
    uint32_t export_count;
    uint32_t library_starts[16];
    uint32_t library_start_count;
    /* the module's thread-local data: each thread's copy starts with filesz bytes from tls_image, then zeros to memsz */
    uint32_t tls_image, tls_filesz, tls_memsz;
    /* code pointers the relocations wrote (Thumb ones, with their bit): where discovery starts beside the exports */
    uint32_t* seeds;
    size_t seed_count;
    unsigned char identity[32]; /* SHA-256 of the image as loaded */
} VitaImage;

/* eboot at path, loaded and relocated. False with error filled: an encrypted SELF (a retail game's, which needs
   decrypting first), a relocation it doesn't know */
bool vita_image_load(const char* path, VitaImage* image, char* error, size_t error_size);
/* Loads a relocatable library at origin and gives its imports module-local SVC numbers. variable_base is the number
   of variable slots the modules loaded before it use; its own are numbered after them. */
bool vita_image_load_at(const char* path, VitaImage* image, uint32_t origin, uint32_t variable_base,
                        char* error, size_t error_size);
/* Appends a loaded library, preserving its guest addresses and import stub metadata. */
bool vita_image_merge(VitaImage* image, VitaImage* library, char* error, size_t error_size);
void vita_image_free(VitaImage* image);
/* the segment holding [address, address + size), NULL for none */
const VitaSegment* vita_image_segment(const VitaImage* image, uint32_t address, uint32_t size);
#endif