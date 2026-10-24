/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A save state's header and thumbnail, which come first in its file, uncompressed (state_header.c): the runtime writes
   and checks them (nx/runtime/state_file.c, state.c), and the launcher lists a game's states by them without reading
   the rest. A state is <game folder>/states/<program>.<slot>.snxstate, the slot a number or "auto" */
#ifndef SNX_STATE_HEADER_H
#define SNX_STATE_HEADER_H
#include <stddef.h>
#include <stdint.h>

#define SNX_STATE_FORMAT   1   /* the file's layout: the header, the thumbnail, the sections and their table */
#define SNX_STATE_HEADER   512 /* the header's bytes, the thumbnail right after them */
#define SNX_STATE_SECTIONS 64  /* a state holds this many sections at most */
#define SNX_STATE_EXT      ".snxstate"

typedef struct {
    uint32_t format;          /* SNX_STATE_FORMAT */
    uint32_t abi;             /* SNX_ABI of the runtime that saved it: a load needs the same */
    uint64_t title_id;        /* the game's */
    uint64_t created;         /* when, in seconds since 1970 (UTC) */
    uint64_t guest_ns;        /* the game's play time then */
    uint32_t thumb_w, thumb_h; /* the thumbnail's size, 0 x 0 for none: thumb_w * thumb_h RGBA8 pixels after the header */
    char runtime[96];         /* the runtime's version, shown, not checked */
    char stamp[64];           /* the game's update and mods (its program's .abi file): a load needs the same */
    char game[128];           /* the game's name, for the launcher */
} SnxStateHeader;

/* the header into b, with where the section table is and how many sections it lists */
void snx_state_header_write(uint8_t b[SNX_STATE_HEADER], const SnxStateHeader* h, uint64_t table_off, uint32_t sections);
/* b read: 0, or -1 with the reason in why when it isn't a state's header this build reads */
int snx_state_header_read(const uint8_t b[SNX_STATE_HEADER], SnxStateHeader* h, uint64_t* table_off,
                          uint32_t* sections, char* why, size_t whysize);
/* The header of the state at path, and when thumb isn't NULL its thumbnail in a new allocation the caller frees (NULL
   for none): 0, or -1 with the reason in why */
int snx_state_peek(const char* path, SnxStateHeader* h, uint8_t** thumb, char* why, size_t whysize);
/* whether a state can be loaded into the game title whose program is stamped stamp (its .abi file, "" for none) by a
   runtime of this SNX_ABI: 0, or -1 with the reason */
int snx_state_fits(const SnxStateHeader* h, uint64_t title, const char* stamp, char* why, size_t whysize);
/* a program's stamp: its .abi file beside it (<program>.abi), the line it holds, "" without one */
void snx_state_stamp(const char* program, char* out, size_t size);
/* a state file's name, program.slot.snxstate, as its program and slot: 1, or 0 for a name of no state */
int snx_state_name(const char* name, char* program, size_t psize, char* slot, size_t ssize);

#endif
