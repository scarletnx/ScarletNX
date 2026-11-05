/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exact reductions of a subset of advanced blend equations to ordinary fixed-function blending. These follow
   the published NV_blend_equation_advanced equations, not the reference driver's implementation. All reductions
   require normalized destination values. Artistic modes and correlated coverage remain unsupported here. */
#ifndef SNX_NVN_BLEND_H
#define SNX_NVN_BLEND_H
#include "nx/gpu/draw_state.h"
#include "nx/gpu/nvn/nvn_obj.h"

static inline int nvn_blend_advanced(DsBlend* blend, u32 target, u64 state) {
    u32 mode = (u32)nvn_field(state, NVN_BLEND_ADV);
    u32 overlap = (u32)nvn_field(state, NVN_BLEND_ADV_OVL);
    int premultiplied = nvn_field(state, NVN_BLEND_ADV_PMS) != 0;
    if (target != 0 || overlap > NVN_BLEND_ADVANCED_OVERLAP_CONJOINT || !nvn_field(state, NVN_BLEND_ADV_NPM)) return 0;
    u8 source = DS_FACTOR_ZERO, destination = DS_FACTOR_ZERO, alpha_source = DS_FACTOR_ZERO;
    switch (mode) {
        case NVN_BLEND_ADVANCED_MODE_ZERO: break;
        case NVN_BLEND_ADVANCED_MODE_SRC:
            source = premultiplied ? DS_FACTOR_ONE : DS_FACTOR_SRC_ALPHA;
            alpha_source = DS_FACTOR_ONE;
            break;
        case NVN_BLEND_ADVANCED_MODE_DST: destination = DS_FACTOR_ONE; break;
        default:
            if (overlap != NVN_BLEND_ADVANCED_OVERLAP_UNCORRELATED) return 0;
            if (!premultiplied && mode != NVN_BLEND_ADVANCED_MODE_SRC_OVER) return 0;
            switch (mode) {
                case NVN_BLEND_ADVANCED_MODE_SRC_OVER:
                    source = premultiplied ? DS_FACTOR_ONE : DS_FACTOR_SRC_ALPHA;
                    alpha_source = DS_FACTOR_ONE;
                    destination = DS_FACTOR_ONE_MINUS_SRC_ALPHA;
                    break;
                case NVN_BLEND_ADVANCED_MODE_DST_OVER:
                    source = alpha_source = DS_FACTOR_ONE_MINUS_DST_ALPHA;
                    destination = DS_FACTOR_ONE;
                    break;
                case NVN_BLEND_ADVANCED_MODE_SRC_IN: source = alpha_source = DS_FACTOR_DST_ALPHA; break;
                case NVN_BLEND_ADVANCED_MODE_DST_IN: destination = DS_FACTOR_SRC_ALPHA; break;
                case NVN_BLEND_ADVANCED_MODE_SRC_OUT: source = alpha_source = DS_FACTOR_ONE_MINUS_DST_ALPHA; break;
                case NVN_BLEND_ADVANCED_MODE_DST_OUT: destination = DS_FACTOR_ONE_MINUS_SRC_ALPHA; break;
                case NVN_BLEND_ADVANCED_MODE_SRC_ATOP:
                    source = alpha_source = DS_FACTOR_DST_ALPHA;
                    destination = DS_FACTOR_ONE_MINUS_SRC_ALPHA;
                    break;
                case NVN_BLEND_ADVANCED_MODE_DST_ATOP:
                    source = alpha_source = DS_FACTOR_ONE_MINUS_DST_ALPHA;
                    destination = DS_FACTOR_SRC_ALPHA;
                    break;
                case NVN_BLEND_ADVANCED_MODE_XOR:
                    source = alpha_source = DS_FACTOR_ONE_MINUS_DST_ALPHA;
                    destination = DS_FACTOR_ONE_MINUS_SRC_ALPHA;
                    break;
                case NVN_BLEND_ADVANCED_MODE_SCREEN:
                    source = alpha_source = DS_FACTOR_ONE;
                    destination = DS_FACTOR_ONE_MINUS_SRC_COLOR;
                    break;
                default: return 0;
            }
    }
    blend->en[target] = 1;
    blend->cop[target] = blend->aop[target] = DS_BLEND_ADD;
    blend->sc[target] = source;
    blend->sa[target] = alpha_source;
    blend->dc[target] = destination;
    blend->da[target] = destination == DS_FACTOR_ONE_MINUS_SRC_COLOR ? DS_FACTOR_ONE_MINUS_SRC_ALPHA : destination;
    return 1;
}
#endif
