/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_PSARC_H
#define SNX_VITA_PSARC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A PSARC archive (the PlayStation archive of FIOS2: big-endian header "PSAR", a table of contents of 30-byte entries,
   the sizes of the blocks, and the blocks, each stored or compressed, of 'block size' bytes of the file). Entry 0 is
   the manifest, the names of the others one to a line */
typedef struct VitaPsarc VitaPsarc;

/* The archive at a host path, or NULL when it is none (the reason is logged for an unsupported one) */
VitaPsarc* vita_psarc_open(const char* host_path);
void vita_psarc_close(VitaPsarc* a);
/* The entry (1 or more) named name, in the archive's own folders: no device, no leading slash, any case. -1 for none */
int vita_psarc_find(const VitaPsarc* a, const char* name);
unsigned vita_psarc_count(const VitaPsarc* a);
uint32_t vita_psarc_toc_length(const VitaPsarc* a);
uint64_t vita_psarc_size(const VitaPsarc* a, int entry);
/* Reads up to length bytes of the entry from offset (decoding the blocks they are in); *got is the count. False when
   a block cannot be read or decoded */
bool vita_psarc_read(const VitaPsarc* a, int entry, uint64_t offset, void* out, size_t length, size_t* got);
#endif
