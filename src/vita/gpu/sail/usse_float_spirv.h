/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The USSE's vector float instructions' back end (usse_float.sail): VDUAL's operands, decoded from its source
   configuration as usse.c decodes the others' (Vita3K's alu.cpp: the layouts of the two operations' sources, the
   internal registers, the unified store's source and their swizzles), moved here from usse.c as they were */

[[maybe_unused]] static void dual_swizzle(uint8_t out[4], unsigned swizzle, bool extended, bool vec4) {
    static const uint8_t v4[2][16][4] = {
        {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(X, Y, Z, W), S4(Y, Z, W, W),
         S4(X, Y, Z, Z), S4(X, X, Y, Z), S4(X, Y, X, Y), S4(X, Y, W, Z), S4(Z, X, Y, W), S4(Z, W, Z, W),
         S4(0, 0, 0, 0), S4(H, H, H, H), S4(1, 1, 1, 1), S4(2, 2, 2, 2)},
        {S4(Y, Z, X, W), S4(Z, W, X, Y), S4(X, Z, W, Y), S4(Y, Y, W, W), S4(W, Y, Z, W), S4(W, Z, W, Z),
         S4(X, Y, Z, X), S4(Z, Z, W, W), S4(X, W, Z, X), S4(Y, Y, Y, X), S4(Y, Y, Y, Z), S4(Z, W, Z, W),
         S4(Y, Z, X, Z), S4(X, X, Y, Y), S4(X, Z, W, W), S4(X, Y, Z, 1)}};
    static const uint8_t v3[2][16][4] = {
        {S3(X, X, X), S3(Y, Y, Y), S3(Z, Z, Z), S3(W, W, W), S3(X, Y, Z), S3(Y, Z, W), S3(X, X, Y), S3(X, Y, X),
         S3(Y, Y, X), S3(Y, Y, Z), S3(Z, X, Y), S3(X, Z, Y), S3(0, 0, 0), S3(H, H, H), S3(1, 1, 1), S3(2, 2, 2)},
        {S3(X, Y, Y), S3(Y, X, Y), S3(X, X, Z), S3(Y, X, X), S3(X, Y, 0), S3(X, 1, 0), S3(X, Z, Y), S3(Y, Z, X),
         S3(Z, Y, X), S3(Z, Z, Y), S3(X, Y, 1), SU, SU, SU, SU, SU}};
    memcpy(out, vec4 ? v4[extended][swizzle & 15] : v3[extended][swizzle & 15], 4);
}

enum { DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_INTERNAL2, DUAL_UNIFIED, DUAL_NONE };

[[maybe_unused]] static bool dual_op1_layout(unsigned count, unsigned config, uint8_t out[3]) {
    static const uint8_t layouts[3][4][3] = {
        {{DUAL_UNIFIED, DUAL_NONE, DUAL_NONE}, {DUAL_INTERNAL0, DUAL_NONE, DUAL_NONE},
         {DUAL_INTERNAL1, DUAL_NONE, DUAL_NONE}, {DUAL_INTERNAL2, DUAL_NONE, DUAL_NONE}},
        {{DUAL_UNIFIED, DUAL_INTERNAL1, DUAL_NONE}, {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_NONE},
         {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_NONE}, {DUAL_NONE, DUAL_NONE, DUAL_NONE}},
        {{DUAL_UNIFIED, DUAL_INTERNAL1, DUAL_INTERNAL2}, {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_INTERNAL2},
         {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_UNIFIED}, {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_INTERNAL2}}};
    if (!count || count > 3 || config > 3) return false;
    memcpy(out, layouts[count - 1][config], 3);
    return out[0] != DUAL_NONE;
}

[[maybe_unused]] static bool dual_op2_layout(unsigned first_count, unsigned count, unsigned config, uint8_t out[3]) {
    if (config > 3) return false;
    out[1] = out[2] = DUAL_NONE;
    if (first_count == 1) {
        if (count == 1) {
            out[0] = config == 0 ? DUAL_INTERNAL0 : DUAL_UNIFIED;
            return true;
        }
        if (count == 2) {
            static const uint8_t layouts[4][3] = {
                {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_NONE}, {DUAL_UNIFIED, DUAL_INTERNAL1, DUAL_NONE},
                {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_NONE}, {DUAL_INTERNAL1, DUAL_UNIFIED, DUAL_NONE}};
            memcpy(out, layouts[config], 3);
            return true;
        }
        if (count == 3) {
            static const uint8_t layouts[4][3] = {
                {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_INTERNAL2}, {DUAL_UNIFIED, DUAL_INTERNAL1, DUAL_INTERNAL2},
                {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_INTERNAL2}, {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_INTERNAL1}};
            memcpy(out, layouts[config], 3);
            return true;
        }
    } else if (first_count == 2) {
        if (count == 1 && config < 3) {
            out[0] = config == 0 ? DUAL_INTERNAL0 : config == 1 ? DUAL_INTERNAL1 : DUAL_UNIFIED;
            return true;
        }
        if (count == 2 && config < 3) {
            static const uint8_t layouts[3][3] = {
                {DUAL_INTERNAL0, DUAL_INTERNAL2, DUAL_NONE}, {DUAL_INTERNAL1, DUAL_INTERNAL2, DUAL_NONE},
                {DUAL_INTERNAL2, DUAL_UNIFIED, DUAL_NONE}};
            memcpy(out, layouts[config], 3);
            return true;
        }
    } else if (first_count == 3 && count == 1) {
        out[0] = config == 0 ? DUAL_INTERNAL0 : config == 1 ? DUAL_INTERNAL1 : config == 2 ? DUAL_INTERNAL2 : DUAL_UNIFIED;
        return true;
    }
    return false;
}

[[maybe_unused]] static Operand dual_source(T* t, unsigned id, unsigned first_count, const arg_VDUAL* arg,
                           Type type, bool vector_load) {
    unsigned swizzle, number;
    Operand op = {0, B_INTERNAL, 0, {SW_X, SW_Y, SW_Z, SW_W}, type};
    switch (id) {
        case DUAL_UNIFIED:
            op = decode_src12(t, arg->unified_store_slot_num, arg->unified_store_slot_bank, false, vector_load,
                              vector_load ? 8 : 7);
            dual_swizzle(op.swizzle, arg->unified_store_swiz, first_count < 2 && arg->gpi2_slot_num_bit_1,
                         arg->comp_count_type);
            if (first_count < 2 && arg->gpi2_slot_num_bit_0_or_unified_store_abs) op.flags |= F_ABS;
            if (arg->unified_store_neg) op.flags |= F_NEG;
            break;
        case DUAL_INTERNAL0:
            number = arg->gpi0_slot_num, swizzle = arg->gpi0_swiz;
            op.num = number;
            dual_swizzle(op.swizzle, swizzle, false, arg->comp_count_type);
            break;
        case DUAL_INTERNAL1:
            number = arg->gpi1_slot_num, swizzle = arg->gpi1_swiz;
            op.num = number;
            dual_swizzle(op.swizzle, swizzle, arg->gpi1_swiz_ext, arg->comp_count_type);
            if (arg->gpi1_neg) op.flags |= F_NEG;
            break;
        case DUAL_INTERNAL2:
            op.num = first_count >= 2
                         ? ((arg->gpi2_slot_num_bit_1 << 1) | arg->gpi2_slot_num_bit_0_or_unified_store_abs)
                         : 2;
            break;
        default:
            fail(t, "VDUAL source layout");
            break;
    }
    op.type = type;
    return op;
}

/* the layouts of both operations' sources (their counts and the configuration), false where there is none */
[[maybe_unused]] static int sg_DualLayouts(SgG* g, int64_t first, int64_t second, int64_t config) {
    return dual_op1_layout((unsigned)first, (unsigned)config, g->layout[0]) &&
           dual_op2_layout((unsigned)first, (unsigned)second, (unsigned)config, g->layout[1]);
}
/* operand k: operation op's (1 or 2) source i */
[[maybe_unused]] static void sg_DualSource(SgG* g, int64_t k, int64_t op, int64_t i, int64_t first_count, int64_t type,
                                           int64_t vector_load) {
    g->op[k] = dual_source(g->t, g->layout[op - 1][i], (unsigned)first_count, g->arg, (Type)type, vector_load);
}
