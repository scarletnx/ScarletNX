/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ldr:ro / ro:1, IRoInterface. Behavioral reference: Eden's service/ro/ro.cpp and
   cmif_serialization.h (see docs/platforms/nx/nro.md). The guest nn::ro linker, not this service, resolves symbols,
   applies relocations and calls constructors. An NRO the recompiler found in the game's RomFS has its code made ahead
   of time for one guest address (g_nros, nx/loader/nro_slots.h): it is mapped there, found by the SHA-256 of its image,
   and its own AOT module runs it (runtime/guest_jit.c's jit_nro_added). Any other NRO, and one whose address is in
   use, is mapped where there is room and its code is the runtime JIT's (its text joins the JIT's images while it is
   mapped): AOT functions made for one address cannot be rebased by changing their dispatch entries. */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "hle/services/ldr.swipc.inc"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include <openssl/evp.h>
#include "nx/runtime/nce/nce.h"
#include "common/utilities/endian.h"

/* each session's RoState, the lock and the session that initialized: g_hle.ldr (services_int.h) */

static int ro_range(u64 a, u64 size, int nonzero) {
    R_UNLESS(!(a & 0xFFF), SNX_RC_RO_INVALID_ADDRESS);
    if ((nonzero && !size) || (size & 0xFFF) || size > ~a) R_THROW(SNX_RC_RO_INVALID_SIZE);
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
    R_UNLESS(slot, SNX_RC_RO_MAX_REGISTRATION);
    /* An Ounce NRR's fields lie 0x180 bytes further on than a Switch's, past longer keys and signatures (read in one
       game's file: the first hash at 0x4d0, the size at 0x4b8) */
    u32 shift = g_ounce ? 0x180 : 0, header_size = 0x350 + shift;
    R_UNLESS(size >= header_size && size <= 0xFFFFFFFFull && ro_readable(a, size), SNX_RC_RO_INVALID_NRR_IMAGE);
    u8 header[0x350 + 0x180];
    memcpy(header, GP(a), header_size);
    u32 off = snx_ld_le32(header + 0x340 + shift), count = snx_ld_le32(header + 0x344 + shift);
    if (memcmp(header, "NRR0", 4) || snx_ld_le32(header + 0x338 + shift) != size ||
        snx_ld_le64(header + 0x330 + shift) != g_hle.kernel.title_id || off < header_size || off > size ||
        count > (size - off) / 32)
        R_THROW(SNX_RC_RO_INVALID_NRR_IMAGE);
    R_UNLESS(header[0x33c + shift] == kind, SNX_RC_RO_INVALID_NRR_KIND);
    /* Like Eden, no RSA trust-chain verification. Snapshot the hashes, not pointers to mutable guest storage. */
    u8* hashes = malloc(count ? (size_t)count * 32 : 1);
    R_UNLESS(hashes, SNX_RC_RO_INTERNAL_ERROR);
    memcpy(hashes, GP(a + off), (size_t)count * 32);
    *slot = (RoNrr){a, count, hashes};
    R_SUCCEED();
}
static int ro_unregister_nrr(RoState* s, u64 a) {
    R_UNLESS(!(a & 0xFFF), SNX_RC_RO_INVALID_ADDRESS);
    for (int i = 0; i < RO_LIMIT; i++) {
        RoNrr* n = &s->nrr[i];
        if (!n->address || n->address != a) continue;
        free(n->hashes);
        memset(n, 0, sizeof *n);
        R_SUCCEED();
    }
    R_THROW(SNX_RC_RO_NOT_REGISTERED);
}
static void ro_hash(const u8* image, u64 size, u8* hash) {
    if (!EVP_Digest(image, (size_t)size, hash, NULL, EVP_sha256(), NULL))
        fatal("ldr: SHA-256 failed (out of memory)\n");
}
/* hash: the SHA-256 of the image as it is mapped, which an NRR must hold */
static int ro_validate_nro(RoState* s, const MemCodeMap* m, u32* text, u32* ro, u32* data, u8* hash) {
    const u8* h = GP(m->base);
    *text = snx_ld_le32(h + 0x24);
    *ro = snx_ld_le32(h + 0x2c);
    *data = snx_ld_le32(h + 0x34);
    if (memcmp(h + 0x10, "NRO0", 4) || (snx_ld_le32(h + 0x1c) & 2) || snx_ld_le32(h + 0x18) != m->size ||
        snx_ld_le32(h + 0x38) != m->bss_size || ((*text | *ro | *data) & 0xFFF) || *text < 0x1000 ||
        snx_ld_le32(h + 0x20) || snx_ld_le32(h + 0x28) != *text || snx_ld_le32(h + 0x30) != (u64)*text + *ro ||
        (u64)*text + *ro + *data != m->size)
        R_THROW(SNX_RC_RO_INVALID_NRO_IMAGE);
    ro_hash(h, m->size, hash);
    int authorized = 0;
    for (int i = 0; i < RO_LIMIT && !authorized; i++)
        for (u32 j = 0; j < s->nrr[i].count; j++)
            if (!memcmp(s->nrr[i].hashes + (size_t)j * 32, hash, 32)) {
                authorized = 1;
                break;
            }
    R_UNLESS(authorized, SNX_RC_RO_NOT_AUTHORIZED);
    for (int i = 0; i < RO_LIMIT; i++)
        if (s->nro[i].map.base && !memcmp(s->nro[i].id, h + 0x40, 32)) R_THROW(SNX_RC_RO_NRO_ALREADY_LOADED);
    R_SUCCEED();
}
static int ro_load_nro(RoState* s, u64 source, u64 size, u64 bss, u64 bss_size, u64* address) {
    MemCodeMap m = {0, source, size, bss, bss_size};
    int r;
    R_TRY(ro_range(m.source, m.size, 1));
    R_TRY(ro_range(m.bss, m.bss_size, 0));
    R_UNLESS(m.bss_size <= ~m.size, SNX_RC_RO_INVALID_SIZE);
    RoNro* slot = NULL;
    for (int i = 0; i < RO_LIMIT; i++)
        if (!s->nro[i].map.base) {
            slot = &s->nro[i];
            break;
        }
    R_UNLESS(slot, SNX_RC_RO_TOO_MANY_NRO);
    /* The address the recompiler made this NRO's code for, by the hash of its image where the game holds it: the view
       goes there, and nowhere else in the addresses kept for such NROs. When that address is in use (the NRO mapped
       twice at once, through another session, or other memory the game put there), this mapping goes where there is
       room and the JIT runs it */
    u8 held[32], hash[32];
    long place = -1;
    u64 taken = 0;
    if (g_nros && m.size >= 0x80 && ro_readable(m.source, m.size)) {
        ro_hash(GP(m.source), m.size, held);
        place = snx_nro_slot(g_nros, held);
        if (place >= 0 && g_nros->slots[place].size != m.size + m.bss_size) place = -1;
    }
    if (place >= 0) {
        m.base = g_nros->slots[place].base;
        r = mem_map_code(&m);
        if (r == MEM_LIMIT) {
            taken = m.base;
            m.base = 0;
            r = mem_map_code(&m);
        }
    } else
        r = mem_map_code(&m);
    if (r)
        R_THROW(r == MEM_LIMIT      ? SNX_RC_RO_OUT_OF_ADDRESS_SPACE
                : r == MEM_BADSTATE ? SNX_RC_RO_INVALID_MEMORY_STATE
                                    : SNX_RC_RO_INTERNAL_ERROR);
    u32 text, ro, data;
    r = ro_validate_nro(s, &m, &text, &ro, &data, hash);
    if (R_SUCCEEDED(r)) {
        if (mem_set_state(m.base, text, MS_ALIASCODE, PERM_RX) ||
            (ro && mem_set_state(m.base + text, ro, MS_ALIASCODE, PERM_R)) ||
            ((data || m.bss_size) &&
             mem_set_state(m.base + text + ro, (u64)data + m.bss_size, MS_ALIASCODEDATA, PERM_RW)))
            r = SNX_RC_RO_INTERNAL_ERROR;
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
    nce_code_added(m.base, text); /* before the game runs it: an SVC would be a system call of the host */
    *address = m.base;
    /* its own AOT module, when the image mapped at its address is the one that was hashed (the game may have written
       its heap since); then the JIT, for the code that module lacks, or all of it */
    u32 entries = 0;
    int own = place >= 0 && !taken && !memcmp(hash, held, 32) &&
              !jit_nro_added(hash, m.base, m.size + m.bss_size, slot->id, &entries);
    int jit = jit_code_added(m.base, text, m.size);
    if (taken) {
        char name[65];
        for (int i = 0; i < 32; i++) snprintf(name + 2 * i, 3, "%02x", held[i]);
        WARN("ldr:ro: the NRO %s is mapped at %llx and the JIT runs it: %llx, the address its AOT module is made for, "
             "is in use\n",
             name, (unsigned long long)m.base, (unsigned long long)taken);
    }
    if (own)
        LOG("ldr:ro: mapped NRO at %llx (%llu bytes, its AOT module, %u entries)\n", (unsigned long long)m.base,
            (unsigned long long)(m.size + m.bss_size), (unsigned)entries);
    else
        LOG("ldr:ro: mapped NRO at %llx (%llu bytes, %s)\n", (unsigned long long)m.base,
            (unsigned long long)(m.size + m.bss_size), jit ? "no AOT variants" : "AOT variants");
    R_SUCCEED();
}
static int ro_unload_nro(RoState* s, u64 base) {
    R_UNLESS(!(base & 0xFFF), SNX_RC_RO_INVALID_ADDRESS);
    for (int i = 0; i < RO_LIMIT; i++) {
        RoNro* n = &s->nro[i];
        if (!n->map.base || n->map.base != base) continue;
        jit_code_removed(base); /* before its memory goes: a hash may be reading it */
        jit_nro_removed(base);
        R_UNLESS(!mem_unmap_code(&n->map), SNX_RC_RO_INVALID_MEMORY_STATE);
        memset(n, 0, sizeof *n);
        R_SUCCEED();
    }
    R_THROW(SNX_RC_RO_NOT_LOADED);
}
/* every command but RegisterProcessHandle needs the session initialized, and all of them the client's process id */
static int ro_session(SvcObj* self, u64 pid) {
    RoState* s = self->data;
    R_UNLESS(pid == K_PROCESS_ID && s->initialized, SNX_RC_RO_INVALID_PROCESS);
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
    R_UNLESS(pid == K_PROCESS_ID && ro_process_handle(process), SNX_RC_RO_INVALID_PROCESS);
    R_UNLESS(!s->initialized && !g_hle.ldr.owner, SNX_RC_RO_INVALID_SESSION);
    s->initialized = 1;
    g_hle.ldr.owner = s->obj;
    R_SUCCEED();
}
static int nn_ro_detail_IRoInterface_RegisterProcessModuleInfo(SvcObj* self, u64 pid, u64 nrr_address, u64 nrr_size,
                                                               u32 process) {
    RoState* s = self->data;
    R_TRY(ro_session(self, pid));
    R_UNLESS(ro_process_handle(process), SNX_RC_RO_INVALID_PROCESS);
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
        if (s->nro[i].map.base) jit_nro_removed(s->nro[i].map.base);
        if (s->nro[i].map.base && mem_unmap_code(&s->nro[i].map))
            WARN("ldr:ro: the NRO at %#llx (%#llx bytes) is no longer mapped as its code: it stays\n",
                 (unsigned long long)s->nro[i].map.base, (unsigned long long)s->nro[i].map.size);
    }
    if (g_hle.ldr.owner == s->obj) g_hle.ldr.owner = 0;
    pm_unlock(&g_host.ldr.lock);
}
static SvcObj* ro_new(const char* name, int kind) {
    SvcObj* o = svcobj_new_data(name, ro_dispatch, g_hle.ldr.states, sizeof *g_hle.ldr.states, RO_SESSIONS,
                                &g_hle.ldr.state_next, "ldr:ro sessions");
    ((RoState*)o->data)->kind = kind;
    o->destroy = ro_destroy;
    return o;
}
SvcObj* new_ldr(void) { return ro_new("ldr:ro", 0); }
SvcObj* new_ro1(void) { return ro_new("ro:1", 1); }

/* the runtime's own state (services.h): no session, no process to name */
static RoState* ro_own(void) { return &g_hle.ldr.states[RO_SESSIONS]; }
void ldr_own_kind(int kind) {
    pm_lock(&g_host.ldr.lock);
    ro_own()->kind = kind;
    pm_unlock(&g_host.ldr.lock);
}
int ldr_own_register(u64 nrr, u64 size, const u32* process) {
    int r = process && !ro_process_handle(*process) ? SNX_RC_RO_INVALID_PROCESS : 0;
    if (r) return r;
    pm_lock(&g_host.ldr.lock);
    r = ro_register_nrr(ro_own(), nrr, size, process ? ro_own()->kind : 0);
    pm_unlock(&g_host.ldr.lock);
    return r;
}
int ldr_own_unregister(u64 nrr) {
    int r;
    pm_lock(&g_host.ldr.lock);
    r = ro_unregister_nrr(ro_own(), nrr);
    pm_unlock(&g_host.ldr.lock);
    return r;
}
int ldr_own_load(u64 nro, u64 size, u64 bss, u64 bss_size, u64* address) {
    int r;
    pm_lock(&g_host.ldr.lock);
    r = ro_load_nro(ro_own(), nro, size, bss, bss_size, address);
    pm_unlock(&g_host.ldr.lock);
    return r;
}
int ldr_own_unload(u64 address) {
    int r;
    pm_lock(&g_host.ldr.lock);
    r = ro_unload_nro(ro_own(), address);
    pm_unlock(&g_host.ldr.lock);
    return r;
}
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_ldr_state_fns, STATE_FN("ldr", ro_dispatch), STATE_FN("ldr", ro_destroy));
