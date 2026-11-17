/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The generated test dispatchers compiled as strict ISO C23 (tests/CMakeLists.txt): a service's file includes its .inc and
   defines the handlers, as this one does with handlers that do nothing */
#include "tests/swipc/other.swipc.inc"
#include "tests/swipc/test.swipc.inc"

static int test_IThing_Layout(SvcObj* self, u32 a, u64 b, u8 c, u16 d, const test_Small* e, u32 f, u8* x, u64* y,
                              u16* z) {
    (void)self, (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)x, (void)y, (void)z;
    return 0;
}
static int test_IThing_Pid(SvcObj* self, u32 a, u64 pid, u64 b, u64* pid_out) {
    (void)self, (void)a, (void)pid, (void)b, (void)pid_out;
    return 0;
}
static int test_IThing_Aruid(SvcObj* self, u64 aruid, u32 v, test_Id* id) {
    (void)self, (void)aruid, (void)v, (void)id;
    return 0;
}
static int test_IThing_Buffers(SvcObj* self, IpcBuf a0, IpcBuf auto0, IpcBuf x1, IpcBuf b0, IpcBuf auto1, IpcBuf c1,
                               IpcBuf x2, IpcBuf b2) {
    (void)self, (void)a0, (void)auto0, (void)x1, (void)b0, (void)auto1, (void)c1, (void)x2, (void)b2;
    return 0;
}
static int test_IThing_Objects(SvcObj* self, SvcObj* thing, u32* m, SvcObj** out, u32* h) {
    (void)self, (void)thing, (void)m, (void)out, (void)h;
    return 0;
}
static int test_IThing_Fail(SvcObj* self, SvcObj** o, u32* h, u32* v) {
    (void)self, (void)o, (void)h, (void)v;
    return 1;
}
static int test_IThing_InHandles(SvcObj* self, u32 a, u32 b, u32 c) {
    (void)self, (void)a, (void)b, (void)c;
    return 0;
}
static int test_IThing_Struct(SvcObj* self, const test_Pair* p, u8 q, const test_Uid* u, const u8* raw, test_Pair* r,
                              u8* quad) {
    (void)self, (void)p, (void)q, (void)u, (void)raw, (void)r, (void)quad;
    return 0;
}
static int test_IThing_Empty(SvcObj* self) {
    (void)self;
    return 0;
}
static int test_IThing_Small(SvcObj* self, u8* flag) {
    (void)self, (void)flag;
    return 0;
}
static int test_IThing_Big(SvcObj* self, u8* big) {
    (void)self, (void)big;
    return 0;
}
static int test_IThing_ObjectFail(SvcObj* self, SvcObj* thing, u32* v) {
    (void)self, (void)thing, (void)v;
    return 1;
}
static int test_IThing_RawFail(SvcObj* self, u32 a, u32* v) {
    (void)self, (void)a, (void)v;
    return 1;
}
static int test_IThing_Unsorted(SvcObj* self, const test_Uid* u, u32 a, u8 b, test_Tail* t, u8* c) {
    (void)self, (void)u, (void)a, (void)b, (void)t, (void)c;
    return 0;
}
static int test_IThing_Nested(SvcObj* self, const test_Nest* n, const i16* s, test_Nest* m, u32* w) {
    (void)self, (void)n, (void)s, (void)m, (void)w;
    return 0;
}
static int test_IThing_FixedOut(SvcObj* self, IpcBuf uid) {
    (void)self, (void)uid;
    return 0;
}
static int test_fallback(SvcObj* self, IpcCtx* c) {
    (void)self, (void)c;
    return 0;
}
static int test_IOther_Swap(SvcObj* self, const test_Pair* p, test_Id* id) {
    (void)self, (void)p, (void)id;
    return 0;
}
