/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Constant buffer words folded into a translation (shader.h, shader_cb_fold_value): what a translation records of
   each load at a constant place, whether a translation still holds for a draw's buffers, and which of a program's
   translations is made next. Pure functions of the records and a reader, with no GPU, so a test checks each case */
#ifndef SNX_VK_CB_FOLD_H
#define SNX_VK_CB_FOLD_H
#include "nx/gpu/vk/vk.h"

/* VkCbRead.ok: what a translation did with a word, and so when it still holds */
enum {
    VK_CB_UNBOUND = 0, /* a read (a BRX's jump table, a texture offset) found no buffer: holds while there is none */
    VK_CB_READ = 1,    /* a read got v: holds while the word is v */
    VK_CB_LOAD = 2,    /* a load at a constant place, left a load: holds whatever the word is */
    VK_CB_FOLD = 3     /* a load at a constant place, made the constant v: holds while the word is v */
};
/* how a translation folds: its draw's words by their value alone, or of those only the words every translation the
   program has agrees on (the same constant in each) */
enum { VK_CB_FOLD_NONE = 0, VK_CB_FOLD_VALUES, VK_CB_FOLD_AGREE };
/* the bytes of bank idx from byte off on, size of them, where a read of them lands nothing of what the GPU holds;
   NULL where it would, or the buffer has no such bytes */
typedef const u8* (*VkCbBytes)(u32 idx, u32 off, u32 size);
typedef struct {
    int mode;
    /* the buffers a translation folds from: a word the GPU holds is no reason to land it, the load just stays */
    VkCbBytes bytes;
    /* VK_CB_FOLD_AGREE: the reads of the program's translations made from a draw's words */
    const VkCbReads* const* others;
    u32 nothers;
} VkCbFold;

/* vk_translate_spirv that folds as fold says (NULL: nothing) what read holds, recording it in reads */
u32* vk_translate_spirv_fold(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, VkCbReader read,
                             VkCbReads* reads, const VkCbFold* fold);
/* A load at a constant place (bank idx, byte off) of the translation that records into reads: 1 with the word in *out
   when the translation holds it as a constant, 0 for a load. The word is read from fold's bytes once a translation
   (a second load of it gets the first one's answer); it folds when shader_cb_fold_value takes it and, for
   VK_CB_FOLD_AGREE, every one of fold's others has it folded to the same word. *failed is set when the record could
   not grow: the translation is then dropped */
int vk_cb_fold_note(VkCbReads* reads, const VkCbFold* fold, u32 idx, u32 off, u32* out, int* failed);
/* vk_cb_reads_hold (vk.h) whose folded words are read from bytes, a bank's at once: the check every draw makes of
   its shaders. A folded word bytes can't give does not hold */
int vk_cb_folds_hold(const VkCbReads* reads, VkCbReader read, VkCbBytes bytes);
/* the reads first, then the folds by bank and offset, the loads after them: vk_cb_folds_hold reads each bank's folds
   from one range of bytes and stops at the first load */
void vk_cb_reads_sort(VkCbReads* reads);
/* the words reads holds as constants */
u32 vk_cb_fold_count(const VkCbReads* reads);
/* 1 when a translation made now for the same program would fold a word that reads left a load: fold's bytes hold a
   word shader_cb_fold_value takes there (and, for VK_CB_FOLD_AGREE, fold's others agree on it) */
int vk_cb_fold_more(const VkCbReads* reads, const VkCbFold* fold);
/* how the next translation of a program folds, after made of them made from a draw's words hold a constant: by
   value up to SH_CB_FOLD_VARIANTS of them, then one of the words they agree on, then none */
int vk_cb_fold_mode(u32 made);
#endif
