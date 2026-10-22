/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A save state's stream, as the parts of the runtime that write their own state into one see it (the GPU, the
   services with heap memory): bytes out on a save, in on a load. state.c makes it over the state's file */
#ifndef SNX_STATE_IO_H
#define SNX_STATE_IO_H
#include "common/cpu/aarch64/cpu.h"
#include <stdint.h>

typedef struct StateIo {
    void* ctx;
    int (*put)(void* ctx, const void* p, u64 n); /* 0, or -1 when the state can't take it */
    int (*get)(void* ctx, void* p, u64 n);       /* 0, or -1 when the state holds fewer or damaged bytes */
} StateIo;

static inline int io_put(StateIo* io, const void* p, u64 n) { return io->put(io->ctx, p, n); }
static inline int io_get(StateIo* io, void* p, u64 n) { return io->get(io->ctx, p, n); }
static inline int io_put32(StateIo* io, u32 v) { return io->put(io->ctx, &v, sizeof v); }
static inline int io_put64(StateIo* io, u64 v) { return io->put(io->ctx, &v, sizeof v); }
static inline int io_get32(StateIo* io, u32* v) { return io->get(io->ctx, v, sizeof *v); }
static inline int io_get64(StateIo* io, u64* v) { return io->get(io->ctx, v, sizeof *v); }

/* A pointer into the runtime's own image (a function, a string literal) as a number a later run of the same build
   maps back, wherever the image was loaded: its distance from io_anchor's code, which state.c defines. Through
   integers, which C converts function pointers to as it does object pointers */
void io_anchor(void);
#define IO_CODE_OFF(p)      ((p) ? (i64)((uintptr_t)(p) - (uintptr_t)io_anchor) : (i64)0)
#define IO_CODE_PTR(T, off) ((off) ? (T)((uintptr_t)io_anchor + (uintptr_t)(off)) : (T)0)

#endif
