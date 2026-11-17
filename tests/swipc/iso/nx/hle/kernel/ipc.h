/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the generated dispatchers use of hle/kernel/ipc.h, in ISO C: tests/CMakeLists.txt compiles the generated test
   dispatcher against it with -std=c23 -pedantic-errors, so the generator's output stays ISO C */
#ifndef SNX_IPC_H
#define SNX_IPC_H
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
void snx_le16_array(void* p, size_t n);
void snx_le32_array(void* p, size_t n);
void snx_le64_array(void* p, size_t n);
typedef struct {
    u64 addr, size;
    u32 flags;
} IpcBuf;
typedef struct IpcCtx {
    u32 cmd;
    const u8* raw;
    u32 raw_size;
    u64 pid;
    int has_pid;
    u32 copy_h[8];
    int ncopy;
    IpcBuf a[8], b[8], w[8], x[8], c[8];
    int na, nb, nw, nx, nc;
    int is_domain, tipc;
    u8 out[0x100];
    u32 out_size;
} IpcCtx;
typedef struct SvcObj SvcObj;
typedef SvcObj* (*SvcCtor)(void);
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*));
SvcObj* svcobj_new_data(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), void* base, size_t size, u32 count,
                        u32* next, const char* what);
#define SVCOBJ_NEW_DATA(name, dispatch, arr, next, what)                                                               \\
    svcobj_new_data((name), (dispatch), (arr), sizeof *(arr), (u32)(sizeof(arr) / sizeof *(arr)), &(next), (what))
void svcobj_unref(SvcObj* o);
void out_copy_h(IpcCtx* c, u32 h);
void out_move_h(IpcCtx* c, u32 h);
void out_obj(IpcCtx* c, SvcObj* o);
int stub_ok(SvcObj* o, IpcCtx* c);
int quiet_ok(SvcObj* o, IpcCtx* c);
int ipc_unknown(SvcObj* o, IpcCtx* c);
void svc_register(const char* name, SvcCtor ctor);
void ipc_in(IpcCtx* c, u32 off, void* dst, u32 n);
void ipc_out(IpcCtx* c, u32 off, const void* src, u32 n);
IpcBuf ipc_buf(const IpcBuf* list, int n, int i);
IpcBuf ipc_buf_auto(const IpcBuf* map, int nmap, int i, const IpcBuf* ptr, int nptr, int j);
int ipc_counts(const IpcCtx* c, int pid, int na, int nb, int nx, int ncopy, u32 raw, u32 tipc_raw);
int ipc_objects(const IpcCtx* c, int nobj);
int ipc_bad_request(SvcObj* o, IpcCtx* c);
int ipc_bad_objects(SvcObj* o, IpcCtx* c);
SvcObj* ipc_in_obj(IpcCtx* c, int i);
#endif
