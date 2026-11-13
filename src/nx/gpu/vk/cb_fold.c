/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Constant buffer words folded into a translation (cb_fold.h) */
#include "nx/gpu/vk/cb_fold.h"
#include <stdlib.h>
#include "common/utilities/endian.h"
#include "nx/gpu/shader/shader.h"

static int cb_fold_word(VkCbBytes bytes, u32 idx, u32 off, u32* out) {
    const u8* p = bytes(idx, off, 4);
    if (p) *out = snx_ld_le32(p);
    return p != NULL;
}

int vk_cb_folds_hold(const VkCbReads* reads, VkCbReader read, VkCbBytes bytes) {
    const VkCbRead* r = reads->r;
    u32 i = 0;
    for (; i < reads->n && r[i].ok <= VK_CB_READ; i++) {
        u32 v = 0;
        int ok = read(r[i].idx, r[i].off, &v);
        if (!ok != (r[i].ok == VK_CB_UNBOUND) || (ok && v != r[i].v)) return 0;
    }
    while (i < reads->n && r[i].ok == VK_CB_FOLD) {
        /* a bank's folds, in the order of their offsets: one range of bytes holds them all */
        u32 first = i, last = i;
        while (last + 1 < reads->n && r[last + 1].ok == VK_CB_FOLD && r[last + 1].idx == r[first].idx) last++;
        const u8* p = bytes(r[first].idx, r[first].off, r[last].off - r[first].off + 4);
        if (!p) return 0;
        for (; i <= last; i++)
            if (snx_ld_le32(p + (r[i].off - r[first].off)) != r[i].v) return 0;
    }
    return 1;
}
/* a translation that folds nothing (a compute program's, a BRX's reads alone): its reads through read */
static const u8* cb_no_bytes(u32 idx, u32 off, u32 size) {
    (void)idx, (void)off, (void)size;
    return NULL;
}
int vk_cb_reads_hold(const VkCbReads* reads, VkCbReader read) { return vk_cb_folds_hold(reads, read, cb_no_bytes); }

/* the load or fold reads holds of a word, NULL for none */
static const VkCbRead* cb_fold_entry(const VkCbReads* reads, u32 idx, u32 off) {
    for (u32 i = 0; i < reads->n; i++) {
        const VkCbRead* c = &reads->r[i];
        if (c->idx == idx && c->off == off && (c->ok == VK_CB_LOAD || c->ok == VK_CB_FOLD)) return c;
    }
    return NULL;
}
/* whether a translation of the draw at hand may hold the word v as a constant */
static int cb_fold_takes(const VkCbFold* fold, u32 idx, u32 off, u32 v) {
    if (!shader_cb_fold_value(v)) return 0;
    for (u32 i = 0; fold->mode == VK_CB_FOLD_AGREE && i < fold->nothers; i++) {
        const VkCbRead* c = cb_fold_entry(fold->others[i], idx, off);
        if (c && (c->ok == VK_CB_LOAD || c->v != v)) return 0;
    }
    return 1;
}

int vk_cb_fold_note(VkCbReads* reads, const VkCbFold* fold, u32 idx, u32 off, u32* out, int* failed) {
    const VkCbRead* seen = cb_fold_entry(reads, idx, off);
    u32 v = 0;
    if (seen) {
        *out = seen->v;
        return seen->ok == VK_CB_FOLD;
    }
    int folds = cb_fold_word(fold->bytes, idx, off, &v) && cb_fold_takes(fold, idx, off, v);
    if (reads->n % 16 == 0) {
        VkCbRead* grown = realloc(reads->r, sizeof *grown * (reads->n + 16));
        if (!grown) {
            *failed = 1;
            return 0;
        }
        reads->r = grown;
    }
    reads->r[reads->n++] = (VkCbRead){idx, off, folds ? VK_CB_FOLD : VK_CB_LOAD, folds ? v : 0};
    *out = v;
    return folds;
}

/* where an entry goes: the reads, the folds, the loads; the folds among themselves by bank and offset */
static int cb_fold_before(const VkCbRead* a, const VkCbRead* b) {
    u32 ka = a->ok <= VK_CB_READ   ? 0
             : a->ok == VK_CB_FOLD ? 1
                                   : 2,
        kb = b->ok <= VK_CB_READ   ? 0
             : b->ok == VK_CB_FOLD ? 1
                                   : 2;
    if (ka != kb) return ka < kb;
    return ka == 1 && (a->idx != b->idx ? a->idx < b->idx : a->off < b->off);
}
void vk_cb_reads_sort(VkCbReads* reads) {
    /* an insertion, which keeps the reads and the loads in the order the translation met them */
    for (u32 i = 1; i < reads->n; i++) {
        VkCbRead c = reads->r[i];
        u32 at = i;
        while (at && cb_fold_before(&c, &reads->r[at - 1])) {
            reads->r[at] = reads->r[at - 1];
            at--;
        }
        reads->r[at] = c;
    }
}

u32 vk_cb_fold_count(const VkCbReads* reads) {
    u32 n = 0;
    for (u32 i = 0; i < reads->n; i++) n += reads->r[i].ok == VK_CB_FOLD;
    return n;
}

int vk_cb_fold_more(const VkCbReads* reads, const VkCbFold* fold) {
    for (u32 i = 0; i < reads->n; i++) {
        const VkCbRead* c = &reads->r[i];
        u32 v = 0;
        if (c->ok == VK_CB_LOAD && cb_fold_word(fold->bytes, c->idx, c->off, &v) &&
            cb_fold_takes(fold, c->idx, c->off, v))
            return 1;
    }
    return 0;
}

int vk_cb_fold_mode(u32 made) {
    return made < SH_CB_FOLD_VARIANTS    ? VK_CB_FOLD_VALUES
           : made == SH_CB_FOLD_VARIANTS ? VK_CB_FOLD_AGREE
                                         : VK_CB_FOLD_NONE;
}
