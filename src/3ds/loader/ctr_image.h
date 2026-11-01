/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_IMAGE_H
#define SNX_CTR_IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Raw ExHeader values: .data bytes may include BSS in homebrew images.
   pages describe initialized page-packed .code; do not copy bytes blindly. */
typedef struct { uint32_t address, pages, bytes; } SnxCtrSegment;
typedef struct {
    SnxCtrSegment segment[3]; /* text, read-only, data; file offsets are page-packed */
    uint32_t bss_size, stack_size;
    uint64_t title_id;
    char name[8];           /* the codeset's name (ExHeader 0x0), not terminated when all eight are used */
    uint32_t main_priority; /* the main thread's priority (ExHeader 0x20F, ARM11 local capabilities), 0 to 63 */
    /* the system modes (ExHeader 0x20D bits 0 to 3 and 0x20E bits 4 to 7): how much FCRAM the application gets on a New
       3DS (0: as its Old 3DS mode says) and on an Old 3DS */
    uint32_t new_mode, old_mode;
    /* the ARM11 kernel capability descriptors (ExHeader 0x370): the address mappings live here */
    uint32_t kernel_caps[28];
    unsigned char identity[32]; /* SHA-256(domain || full ExHeader || expanded .code) */
    unsigned char* code;
    size_t code_size;
} SnxCtrImage;
/* Revalidate raw ExHeader and bounded, exact-sized .code, not an editable INI.
   Only use freshly extracted trees in trusted, non-concurrently-modified host
   directories. Identity is compatibility/integrity, NOT signature authenticity.
   On failure image is zeroed; free a successful image before reading another. */
bool snx_ctr_image_load(const char* directory, SnxCtrImage* image, char* error, size_t error_size);
/* The same image, the same identity, from a game read where it lies (snx_ctr_source_open) */
struct SnxCtrSource;
bool snx_ctr_image_from_source(const struct SnxCtrSource* source, SnxCtrImage* image, char* error, size_t error_size);
void snx_ctr_image_free(SnxCtrImage* image);
#endif
