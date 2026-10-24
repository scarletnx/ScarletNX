/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ldr:ro / ro:1, IRoInterface. Behavioral reference: Eden's service/ro/ro.cpp and
   cmif_serialization.h (see docs/LDR_RO.md). The guest nn::ro linker, not this service, resolves symbols,
   applies relocations and calls constructors. NRO code is the runtime JIT's (runtime/guest_jit.c: its text joins the
   JIT's images while it is mapped), and the interpreter's without a JIT; fixed-address AOT functions cannot safely be
   rebased by changing their dispatch entries. */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "hle/services/ldr.swipc.inc"
#include "nx/runtime/guest_jit.h"
#include <openssl/evp.h>

#define RO_ERROR(d) RC(22, d)

/* each session's RoState, the lock and the session that initialized: g_hle.ldr (services_int.h) */

static u32 ro_u32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 ro_u64(const u8* p) { return ro_u32(p) | (u64)ro_u32(p + 4) << 32; }
static int ro_range(u64 a, u64 size, int nonzero) {
    R_UNLESS(!(a & 0xFFF), RO_ERROR(1025));
    if ((nonzero && !size) || (size & 0xFFF) || size > ~a) R_THROW(RO_ERROR(1026));
    R_SUCCEED();
}
static int ro_readable(u64 a, u64 size) {
    if (a < AS_BASE || a >= AS_SIZE || size > AS_SIZE - a) return 0;
    u64 end = a + size;
    while (a < end) {
        MemBlock b;
        mem_query(a, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !(b.perm & PERM_R) || b.addr > a ||
            b.size > AS_SIZE - b.addr || b.addr + b.size <= a)
            return 0;
        a = b.addr + b.size;
    }
    return 1;
}
static int ro_process_handle(u32 h) {
    KObj* p = h_get_ref(h, KO_PROCESS);
    if (!p) return 0;
    kobj_unref(p); /* one process; ro never owns the client's copy handle */
    return 1;
}
/* kind: the NRR kind RegisterProcessModuleInfo asks for (the session's), 0 for RegisterModuleInfo */
static int ro_register_nrr(RoState* s, u64 a, u64 size, int kind) {
    R_TRY(ro_range(a, size, 1));
    RoNrr* slot = NULL;
    for (int i = 0; i < RO_LIMIT; i++) {
        if (!s->nrr[i].address && !slot) slot = &s->nrr[i];
    }
    R_UNLESS(slot, RO_ERROR(8));
    R_UNLESS(size >= 0x350 && size <= 0xFFFFFFFFull && ro_readable(a, size), RO_ERROR(6));
    u8 header[0x350];
    memcpy(header, GP(a), sizeof header);
    u32 off = ro_u32(header + 0x340), count = ro_u32(header + 0x344);
    if (memcmp(header, "NRR0", 4) || ro_u32(header + 0x338) != size || ro_u64(header + 0x330) != g_hle.kernel.title_id ||
        off < sizeof header || off > size || count > (size - off) / 32)
        R_THROW(RO_ERROR(6));
    R_UNLESS(header[0x33c] == kind, RO_ERROR(10));
    /* Like Eden, no RSA trust-chain verification. Snapshot the hashes, not pointers to mutable guest storage. */
    u8* hashes = malloc(count ? (size_t)count * 32 : 1);
    R_UNLESS(hashes, RO_ERROR(1023));
    memcpy(hashes, GP(a + off), (size_t)count * 32);
    *slot = (RoNrr){a, count, hashes};
    R_SUCCEED();
}
static int ro_unregister_nrr(RoState* s, u64 a) {
    R_UNLESS(!(a & 0xFFF), RO_ERROR(1025));
    for (int i = 0; i < RO_LIMIT; i++) {
        RoNrr* n = &s->nrr[i];
        if (!n->address || n->address != a) continue;
        free(n->hashes);
        memset(n, 0, sizeof *n);
        R_SUCCEED();
    }
    R_THROW(RO_ERROR(1029));
}
static int ro_validate_nro(RoState* s, const MemCodeMap* m, u32* text, u32* ro, u32* data) {
    const u8* h = GP(m->base);
    *text = ro_u32(h + 0x24);
    *ro = ro_u32(h + 0x2c);
    *data = ro_u32(h + 0x34);
    if (memcmp(h + 0x10, "NRO0", 4) || (ro_u32(h + 0x1c) & 2) || ro_u32(h + 0x18) != m->size ||
        ro_u32(h + 0x38) != m->bss_size || ((*text | *ro | *data) & 0xFFF) || *text < 0x1000 || ro_u32(h + 0x20) ||
        ro_u32(h + 0x28) != *text || ro_u32(h + 0x30) != (u64)*text + *ro || (u64)*text + *ro + *data != m->size)
        R_THROW(RO_ERROR(4));
    u8 hash[32];
    if (!EVP_Digest(h, (size_t)m->size, hash, NULL, EVP_sha256(), NULL)) fatal("ldr: SHA-256 failed (out of memory)\n");
    int authorized = 0;
    for (int i = 0; i < RO_LIMIT && !authorized; i++)
        for (u32 j = 0; j < s->nrr[i].count; j++)
            if (!memcmp(s->nrr[i].hashes + (size_t)j * 32, hash, 32)) {
                authorized = 1;
                break;
            }
    R_UNLESS(authorized, RO_ERROR(9));
    for (int i = 0; i < RO_LIMIT; i++)
        if (s->nro[i].map.base && !memcmp(s->nro[i].id, h + 0x40, 32)) R_THROW(RO_ERROR(3));
    R_SUCCEED();
}
static int ro_load_nro(RoState* s, u64 source, u64 size, u64 bss, u64 bss_size, u64* address) {
    MemCodeMap m = {0, source, size, bss, bss_size};
    int r;
    R_TRY(ro_range(m.source, m.size, 1));
    R_TRY(ro_range(m.bss, m.bss_size, 0));
    R_UNLESS(m.bss_size <= ~m.size, RO_ERROR(1026));
    RoNro* slot = NULL;
    for (int i = 0; i < RO_LIMIT; i++)
        if (!s->nro[i].map.base) {
            slot = &s->nro[i];
            break;
        }
    R_UNLESS(slot, RO_ERROR(7));
    r = mem_map_code(&m);
    if (r) R_THROW(r == MEM_LIMIT ? RO_ERROR(2) : r == MEM_BADSTATE ? RO_ERROR(1027) : RO_ERROR(1023));
    u32 text, ro, data;
    r = ro_validate_nro(s, &m, &text, &ro, &data);
    if (R_SUCCEEDED(r)) {
        if (mem_set_state(m.base, text, MS_ALIASCODE, PERM_RX) ||
            (ro && mem_set_state(m.base + text, ro, MS_ALIASCODE, PERM_R)) ||
            ((data || m.bss_size) &&
             mem_set_state(m.base + text + ro, (u64)data + m.bss_size, MS_ALIASCODEDATA, PERM_RW)))
            r = RO_ERROR(1023);
    }
    if (R_FAILED(r)) {
        if (mem_unmap_code(&m)) {
            /* Keep an exceptional failed rollback tracked so session destruction can retry it. */
            slot->map = m;
            WARN("ldr:ro: the NRO at %#llx (%#llx bytes) is no longer mapped as its code: the session's close tries "
                 "again\n",
                 (unsigned long long)m.base, (unsigned long long)m.size);
        }
        return r;
    }
    slot->map = m;
    memcpy(slot->id, GP(m.base + 0x40), 32);
    *address = m.base;
    LOG("ldr:ro: mapped NRO at %llx (%llu bytes, %s)\n", (unsigned long long)m.base,
        (unsigned long long)(m.size + m.bss_size),
        jit_code_added(m.base, text, m.size) ? "no AOT variants" : "AOT variants");
    R_SUCCEED();
}
static int ro_unload_nro(RoState* s, u64 base) {
    R_UNLESS(!(base & 0xFFF), RO_ERROR(1025));
    for (int i = 0; i < RO_LIMIT; i++) {
        RoNro* n = &s->nro[i];
        if (!n->map.base || n->map.base != base) continue;
        jit_code_removed(base); /* before its memory goes: a hash may be reading it */
        R_UNLESS(!mem_unmap_code(&n->map), RO_ERROR(1027));
        memset(n, 0, sizeof *n);
        R_SUCCEED();
    }
    R_THROW(RO_ERROR(1028));
}
/* every command but RegisterProcessHandle needs the session initialized, and all of them the client's process id */
static int ro_session(SvcObj* self, u64 pid) {
    RoState* s = self->data;
    R_UNLESS(pid == K_PROCESS_ID && s->initialized, RO_ERROR(1031));
    R_SUCCEED();
}
static int nn_ro_detail_IRoInterface_MapManualLoadModuleMemory(SvcObj* self, u64 pid, u64 nro_address, u64 nro_size,
                                                               u64 bss_address, u64 bss_size, u64* address) {
    R_TRY(ro_session(self, pid));
    return ro_load_nro(self->data, nro_address, nro_size, bss_address, bss_size, address);
}
static int nn_ro_detail_IRoInterface_UnmapManualLoadModuleMemory(SvcObj* self, u64 pid, u64 nro_address) {
    R_TRY(ro_session(self, pid));
    return ro_unload_nro(self->data, nro_address);
}
static int nn_ro_detail_IRoInterface_RegisterModuleInfo(SvcObj* self, u64 pid, u64 nrr_address, u64 nrr_size) {
    R_TRY(ro_session(self, pid));
    return ro_register_nrr(self->data, nrr_address, nrr_size, 0);
}
static int nn_ro_detail_IRoInterface_UnregisterModuleInfo(SvcObj* self, u64 pid, u64 nrr_address) {
    R_TRY(ro_session(self, pid));
    return ro_unregister_nrr(self->data, nrr_address);
}
static int nn_ro_detail_IRoInterface_RegisterProcessHandle(SvcObj* self, u64 pid, u32 process) {
    RoState* s = self->data;
    R_UNLESS(pid == K_PROCESS_ID && ro_process_handle(process), RO_ERROR(1031));
    R_UNLESS(!s->initialized && !g_hle.ldr.owner, RO_ERROR(1030));
    s->initialized = 1;
    g_hle.ldr.owner = s->obj;
    R_SUCCEED();
}
static int nn_ro_detail_IRoInterface_RegisterProcessModuleInfo(SvcObj* self, u64 pid, u64 nrr_address, u64 nrr_size,
                                                               u32 process) {
    RoState* s = self->data;
    R_TRY(ro_session(self, pid));
    R_UNLESS(ro_process_handle(process), RO_ERROR(1031));
    return ro_register_nrr(s, nrr_address, nrr_size, s->kind);
}
static int ro_dispatch(SvcObj* o, IpcCtx* c) {
    pm_lock(&g_host.ldr.lock);
    int r = nn_ro_detail_IRoInterface_dispatch(o, c);
    pm_unlock(&g_host.ldr.lock);
    return r;
}
static void ro_destroy(SvcObj* o) {
    RoState* s = o->data;
    pm_lock(&g_host.ldr.lock);
    for (int i = 0; i < RO_LIMIT; i++) {
        free(s->nrr[i].hashes);
        if (s->nro[i].map.base) jit_code_removed(s->nro[i].map.base);
        if (s->nro[i].map.base && mem_unmap_code(&s->nro[i].map))
            WARN("ldr:ro: the NRO at %#llx (%#llx bytes) is no longer mapped as its code: it stays\n",
                 (unsigned long long)s->nro[i].map.base, (unsigned long long)s->nro[i].map.size);
    }
    if (g_hle.ldr.owner == s->obj) g_hle.ldr.owner = 0;
    pm_unlock(&g_host.ldr.lock);
}
static SvcObj* ro_new(const char* name, int kind) {
    SvcObj* o = SVCOBJ_NEW_DATA(name, ro_dispatch, g_hle.ldr.states, g_hle.ldr.state_next, "ldr:ro sessions");
    ((RoState*)o->data)->kind = kind;
    o->destroy = ro_destroy;
    return o;
}
SvcObj* new_ldr(void) { return ro_new("ldr:ro", 0); }
SvcObj* new_ro1(void) { return ro_new("ro:1", 1); }
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_ldr_state_fns,
               STATE_FN("ldr", ro_destroy));
