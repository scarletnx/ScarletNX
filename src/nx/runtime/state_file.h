/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A save state's file (state_file.c): its header, its thumbnail, and its sections, each written and read as a
   stream. What goes in the sections is state.c's */
#ifndef SNX_STATE_FILE_H
#define SNX_STATE_FILE_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/state_header.h" /* the header and the thumbnail, which the launcher reads too */

typedef struct StateWriter StateWriter;
typedef struct StateReader StateReader;

/* A new state at path: it is written to path.part, and moved over path by sw_finish only once it is whole, so a save
   that fails or a crash meanwhile leaves the state that was there. thumb (thumb_w * thumb_h * 4 bytes) may be NULL
   for none. NULL, with the reason in why, when the file can't be made */
StateWriter* sw_open(const char* path, const SnxStateHeader* h, const u8* thumb, char* why, size_t whysize);
/* a section, tagged (up to 8 characters) and with the schema hash of what it holds: the bytes given to sw_put until
   sw_end, compressed. 0, or -1 when writing failed (the writer then only takes sw_abort) */
int sw_begin(StateWriter* w, const char* tag, u64 schema);
int sw_put(StateWriter* w, const void* p, u64 n);
int sw_end(StateWriter* w);
/* the section table and the header written, the file moved over path: 0, or -1 with the reason. Both free w */
int sw_finish(StateWriter* w, char* why, size_t whysize);
void sw_abort(StateWriter* w);

/* A state to load: its header in *h. NULL with the reason in why */
StateReader* sr_open(const char* path, SnxStateHeader* h, char* why, size_t whysize);
/* the section tagged tag, read from its start: 0 with its schema hash in *schema, -1 when the state has none */
int sr_begin(StateReader* r, const char* tag, u64* schema);
/* the section's next n bytes: 0, or -1 when it holds fewer or its data is damaged */
int sr_get(StateReader* r, void* p, u64 n);
u64 sr_left(const StateReader* r); /* the bytes of the section not read yet */
void sr_close(StateReader* r);

#endif
