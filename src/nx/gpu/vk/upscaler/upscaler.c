/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* upscaler.h: each backend's capabilities, the choice among them, and the calls each context goes through. FSR 2 is
   gpu/vk/fsr2's (AMD FidelityFX Super Resolution 2.2.1, MIT, Copyright (c) 2022-2023 Advanced Micro Devices, Inc.), the
   copy and the spatial upscaler are passes.c's, DLSS is dlss.cpp's (NVIDIA's runtime, which the user downloads, through
   NVIDIA Streamline: available only where that runs). Newer FSR and XeSS are described, so the choice and the settings
   know them, and are never available: each needs a runtime that is not built in (docs/layers/gpu/upscalers.md) */
#include "nx/gpu/vk/upscaler/upscaler.h"
#include <stdio.h>
#include <stdlib.h>
#include "nx/gpu/vk/fsr2/fsr2.h"
#include "nx/gpu/vk/upscaler/dlss.h"
#include "nx/gpu/vk/upscaler/passes.h"

/* a reason made of values, for the calling thread until its next call */
static _Thread_local char g_reason[192];
/* the preferred backend's reason, and the first built-in accepted one's, kept while the choice tries the others */
static _Thread_local char g_first[192], g_accepted[192];

static const SnxUpscalerCaps g_caps[SNX_UPSCALERS] = {
    /* FSR 2's range refuses nothing: its quality modes go from 1 to 3 and a dynamic resolution further, and the DLSS
       evaluations it took before the choice checked a range must stay its own */
    [SNX_UPSCALER_FSR2] = {"FSR 2.2.1", SNX_UP_TEMPORAL, SNX_UP_DEPTH | SNX_UP_MOTION,
                           SNX_UP_DEPTH | SNX_UP_MOTION | SNX_UP_JITTER | SNX_UP_EXPOSURE | SNX_UP_REACTIVE,
                           SNX_UP_LDR | SNX_UP_HDR, 0, 1e30f, 1, 0},
    [SNX_UPSCALER_SPATIAL] = {"FSR 1 (EASU, RCAS)", SNX_UP_SPATIAL, 0, 0, SNX_UP_LDR, 1, 4, 1, 1},
    [SNX_UPSCALER_COPY] = {"filtered copy", SNX_UP_COPY, 0, 0, SNX_UP_LDR | SNX_UP_HDR, 0.125f, 16, 0, 1},
    [SNX_UPSCALER_DLSS] = {"DLSS", SNX_UP_TEMPORAL, SNX_UP_DEPTH | SNX_UP_MOTION,
                           SNX_UP_DEPTH | SNX_UP_MOTION | SNX_UP_JITTER | SNX_UP_EXPOSURE | SNX_UP_REACTIVE,
                           SNX_UP_LDR | SNX_UP_HDR, 1, 3, 1, 0},
    [SNX_UPSCALER_FSR_NEW] = {"FSR 3.1 or 4", SNX_UP_TEMPORAL, SNX_UP_DEPTH | SNX_UP_MOTION,
                              SNX_UP_DEPTH | SNX_UP_MOTION | SNX_UP_JITTER | SNX_UP_EXPOSURE | SNX_UP_REACTIVE,
                              SNX_UP_LDR | SNX_UP_HDR, 1, 3, 1, 0},
    [SNX_UPSCALER_XESS] = {"XeSS", SNX_UP_TEMPORAL, SNX_UP_DEPTH | SNX_UP_MOTION,
                           SNX_UP_DEPTH | SNX_UP_MOTION | SNX_UP_JITTER | SNX_UP_EXPOSURE | SNX_UP_REACTIVE,
                           SNX_UP_LDR | SNX_UP_HDR, 1, 3, 1, 0},
};

const SnxUpscalerCaps* snx_upscaler_caps(SnxUpscalerId id) {
    return id >= 0 && id < SNX_UPSCALERS ? &g_caps[id] : NULL;
}

/* the storage images every built-in backend writes (and FSR 2 reads) without a format */
static int formatless(VkPhysicalDevice physical, int read, const char** why) {
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(physical, &f);
    if (f.shaderStorageImageWriteWithoutFormat && (!read || f.shaderStorageImageReadWithoutFormat)) return 1;
    *why = "the GPU can't read and write storage images without a format";
    return 0;
}

int snx_upscaler_available(SnxUpscalerId id, VkPhysicalDevice physical, const char** why) {
    const char* none;
    if (!why) why = &none;
    *why = NULL;
    switch (id) {
        case SNX_UPSCALER_FSR2: return formatless(physical, 1, why);
        case SNX_UPSCALER_SPATIAL:
        case SNX_UPSCALER_COPY: return formatless(physical, 0, why);
        case SNX_UPSCALER_DLSS: return snx_dlss_available(why);
        case SNX_UPSCALER_FSR_NEW:
            *why = "FSR 3.1 and 4 are not built in: they need AMD's FidelityFX API runtime";
            return 0;
        case SNX_UPSCALER_XESS: *why = "XeSS is not built in: it needs Intel's XeSS runtime"; return 0;
        default: *why = "no such upscaler"; return 0;
    }
}

static const char* const kind_word[] = {"", "temporal", "spatial", "", "copy"};
static const char* const input_word[] = {"depth", "motion vectors", "jitter", "an exposure", "a reactive mask"};

/* why backend id can't take n, or NULL when it can */
static const char* refusal(SnxUpscalerId id, VkPhysicalDevice physical, const SnxUpscaleNeeds* n) {
    const SnxUpscalerCaps* c = &g_caps[id];
    const char* why = NULL;
    if (!(c->kind & n->accept)) {
        snprintf(g_reason, sizeof g_reason, "%s is a %s upscaler, which the caller doesn't take", c->name,
                 kind_word[c->kind]);
        return g_reason;
    }
    if (!snx_upscaler_available(id, physical, &why)) return why;
    uint32_t missing = c->needs & ~n->given;
    for (uint32_t i = 0; i < 5; i++)
        if (missing >> i & 1) {
            snprintf(g_reason, sizeof g_reason, "%s needs %s, which the caller doesn't give", c->name, input_word[i]);
            return g_reason;
        }
    if (!(c->range & n->range)) {
        snprintf(g_reason, sizeof g_reason, "%s takes no %s color", c->name, n->range & SNX_UP_HDR ? "HDR" : "SDR");
        return g_reason;
    }
    VkFormatProperties fp;
    if (n->output_format) {
        vkGetPhysicalDeviceFormatProperties(physical, n->output_format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            snprintf(g_reason, sizeof g_reason, "%s writes a storage image, which the device can't of format %d",
                     c->name, (int)n->output_format);
            return g_reason;
        }
    }
    if (c->filtered && n->color) {
        vkGetPhysicalDeviceFormatProperties(physical, n->color, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
            snprintf(g_reason, sizeof g_reason, "%s filters the color, which the device can't for format %d", c->name,
                     (int)n->color);
            return g_reason;
        }
    }
    for (uint32_t a = 0; a < 2; a++) {
        float scale = n->render[a] ? (float)n->output[a] / (float)n->render[a] : 0;
        if (scale < c->scale_min || scale > c->scale_max) {
            snprintf(g_reason, sizeof g_reason, "%s scales from %g to %g times, not %ux%u to %ux%u", c->name,
                     (double)c->scale_min, (double)c->scale_max, n->render[0], n->render[1], n->output[0],
                     n->output[1]);
            return g_reason;
        }
    }
    return NULL;
}

SnxUpscalerId snx_upscaler_choose(SnxUpscalerId preferred, VkPhysicalDevice physical, const SnxUpscaleNeeds* n,
                                  const char** why) {
    static const SnxUpscalerId best[] = {SNX_UPSCALER_DLSS, SNX_UPSCALER_FSR_NEW, SNX_UPSCALER_XESS,
                                         SNX_UPSCALER_FSR2, SNX_UPSCALER_SPATIAL, SNX_UPSCALER_COPY};
    const char *none, *first = NULL, *accepted = NULL;
    if (!why) why = &none;
    *why = NULL;
    for (int i = -1; i < (int)(sizeof best / sizeof *best); i++) {
        SnxUpscalerId id = i < 0 ? preferred : best[i];
        if (id < 0 || id >= SNX_UPSCALERS || (i >= 0 && id == preferred)) continue;
        const char* r = refusal(id, physical, n);
        if (!r) {
            *why = first; /* the preferred one's reason, when another stands in */
            return id;
        }
        if (i < 0) {
            snprintf(g_first, sizeof g_first, "%s", r);
            first = g_first;
        }
        /* the reason of the first one the caller would take that is built in, for a call none takes */
        if (!accepted && (g_caps[id].kind & n->accept) && snx_upscaler_available(id, physical, NULL)) {
            snprintf(g_accepted, sizeof g_accepted, "%s", r);
            accepted = g_accepted;
        }
    }
    *why = first ? first : accepted ? accepted : "no upscaler the caller takes is available";
    return SNX_UPSCALERS;
}

/* ---- the contexts */
struct SnxUpscaler {
    SnxUpscalerId id;
    SnxFsr2* fsr;
    SnxUpPasses* passes;
    SnxDlss* dlss;
};

SnxUpscaler* snx_upscaler_create(SnxUpscalerId id, const SnxUpscalerInfo* info, const char** why) {
    const char* none;
    if (!why) why = &none;
    *why = NULL;
    if (!snx_upscaler_available(id, info->physical, why)) return NULL;
    SnxUpscaler* u = (SnxUpscaler*)calloc(1, sizeof *u);
    if (!u) {
        *why = "no memory";
        return NULL;
    }
    u->id = id;
    if (id == SNX_UPSCALER_FSR2) {
        SnxFsr2Info fi = {info->physical,
                          info->device,
                          {info->render_max[0], info->render_max[1]},
                          {info->output[0], info->output[1]},
                          info->flags};
        u->fsr = snx_fsr2_create(&fi);
        if (!u->fsr) {
            snprintf(g_reason, sizeof g_reason, "FSR 2 can't be set up (FSR error %#x)", (unsigned)snx_fsr2_error());
            *why = g_reason;
        }
    } else if (id == SNX_UPSCALER_DLSS)
        u->dlss = snx_dlss_create(info, why);
    else
        u->passes = snx_up_passes_create(info, id == SNX_UPSCALER_SPATIAL, why);
    if (!u->fsr && !u->passes && !u->dlss) {
        free(u);
        return NULL;
    }
    return u;
}

int snx_upscaler_evaluate(SnxUpscaler* u, const SnxUpscaleEval* e, const char** why) {
    const char* none;
    if (!why) why = &none;
    *why = NULL;
    if (u->dlss) return snx_dlss_evaluate(u->dlss, e, why);
    if (!u->fsr) return snx_up_passes_record(u->passes, e, why);
    if (!e->depth.image || !e->motion.image) {
        *why = "FSR 2 needs depth and motion vectors";
        return 0;
    }
#define IMAGE(i) ((SnxFsr2Image){(i).image, (i).view, (i).format, (i).width, (i).height})
    SnxFsr2Dispatch d = {.cb = e->cb,
                         .color = IMAGE(e->color),
                         .depth = IMAGE(e->depth),
                         .motion = IMAGE(e->motion),
                         .exposure = IMAGE(e->exposure),
                         .reactive = IMAGE(e->reactive),
                         .output = IMAGE(e->output),
                         .render = {e->render[0], e->render[1]},
                         .jitter = {e->jitter[0], e->jitter[1]},
                         .motion_scale = {e->motion_scale[0], e->motion_scale[1]},
                         .pre_exposure = e->pre_exposure,
                         .frame_ms = e->frame_ms,
                         .near_plane = e->near_plane > 0 ? e->near_plane : 0.1f,
                         .far_plane = e->far_plane > 0 ? e->far_plane : 1000.0f,
                         .fov_y = e->fov_y > 0 ? e->fov_y : 1.0f,
                         .reset = e->reset,
                         .sharpness = e->sharpness};
#undef IMAGE
    if (snx_fsr2_dispatch(u->fsr, &d)) return 1;
    snprintf(g_reason, sizeof g_reason, "FSR 2's dispatch failed (FSR error %#x)", (unsigned)snx_fsr2_error());
    *why = g_reason;
    return 0;
}

void snx_upscaler_destroy(SnxUpscaler* u) {
    if (!u) return;
    snx_fsr2_destroy(u->fsr);
    snx_up_passes_destroy(u->passes);
    snx_dlss_destroy(u->dlss);
    free(u);
}

SnxUpscalerId snx_upscaler_id(const SnxUpscaler* u) { return u->id; }
