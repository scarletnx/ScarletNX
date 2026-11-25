/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The USSE's tests' back end (usse_test.sail): their results, and the predicate register VTST writes (store_predicate,
   moved here from usse.c as it was) */

[[maybe_unused]] static void store_predicate(T* t, unsigned n, uint32_t cond) {
    uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_PRIVATE, t->t_bool);
    SPV_VOID(&t->e, SPV_OP_STORE, SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->bank[B_PRED], cu(t, n & 3)), cond);
}
[[maybe_unused]] static void sg_SetTest(SgG* g, int64_t i, SgV v) { g->test[i] = v.id; }
[[maybe_unused]] static SgV sg_TestAt(SgG* g, int64_t i) { return sg_v(g->test[i], SGT_BOOL); }
[[maybe_unused]] static void sg_PredWrite(SgG* g, int64_t n, SgV v) { store_predicate(g->t, (unsigned)n, v.id); }
