/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/nvn_command_gap.h"
#undef NDEBUG /* the checks are the test */
#include <assert.h>
#include <string.h>

typedef struct {
    u32 methods[12], values[12], count;
} CapturedMethods;

static void capture(u32 sub, u32 method, u32 value, void* context) {
    CapturedMethods* captured = context;
    assert(sub == 0 && captured->count < 12);
    captured->methods[captured->count] = method;
    captured->values[captured->count++] = value;
}

int main(void) {
    /* Observed TotK cursor gap: two state packets followed by its 1,656-index MME draw. */
    const u32 packet[] = {0x20020701, 6, 0x22ad0000, 0x200207c0, 6, 0x22ad0d8b, 0xa0050e38, 4, 6, 0x22ad0d90, 1, 1656};
    NvnGapIndexedDraw draw = {0};
    assert(nvn_command_gap_indexed_draw(packet, 12, &draw));
    assert(draw.primitive == 4 && draw.index_type == 1 && draw.count == 1656);
    assert(draw.index_address == 0x622ad0d90ull);
    assert(nvn_command_gap_modeled_indexed_draw(packet, 12));
    assert(nvn_command_gap_modeled_indexed_draw(packet + 6, 6));

    CapturedMethods methods = {0};
    assert(nvn_command_gap_methods(packet, 12, capture, &methods));
    assert(methods.count == 9);
    assert(methods.methods[0] == 0x701 && methods.values[0] == 6);
    assert(methods.methods[1] == 0x702 && methods.values[1] == 0x22ad0000);
    assert(methods.methods[2] == 0x7c0 && methods.values[2] == 6);
    assert(methods.methods[3] == 0x7c1 && methods.values[3] == 0x22ad0d8b);
    assert(methods.methods[4] == 0xe38 && methods.values[4] == 4);
    assert(methods.methods[5] == 0xe39 && methods.values[5] == 6);
    assert(methods.methods[8] == 0xe39 && methods.values[8] == 1656);

    /* Increasing, non-increasing, and inline-value packets can share one validated gap. */
    const u32 state_packets[] = {0x200308e0, 0x100, 6, 0x22ad0000, 0x60010904, 0x31, 0x800108e3};
    methods.count = 0;
    assert(nvn_command_gap_methods(state_packets, 7, capture, &methods));
    assert(methods.count == 5);
    assert(methods.methods[0] == 0x8e0 && methods.values[0] == 0x100);
    assert(methods.methods[1] == 0x8e1 && methods.values[1] == 6);
    assert(methods.methods[2] == 0x8e2 && methods.values[2] == 0x22ad0000);
    assert(methods.methods[3] == 0x904 && methods.values[3] == 0x31);
    assert(methods.methods[4] == 0x8e3 && methods.values[4] == 1);
    const u32 binding_packets[] = {0x200308e0, 0x100, 6, 0x22ad0000, 0x60010904, 0x31, 0x60010924, 0x41};
    assert(nvn_command_gap_no_writes(binding_packets, 8));
    const u32 other_stage_bindings[] = {0x6001090c, 0x31, 0x60010914, 0x41, 0x6001091c, 0x51};
    assert(nvn_command_gap_no_writes(other_stage_bindings, 6));
    assert(nvn_command_gap_no_writes(state_packets, 7)); /* CBUF offset is register state. */
    const u32 clip_packet[] = {0x60010544, 0xff};
    methods.count = 0;
    assert(nvn_command_gap_methods(clip_packet, 2, capture, &methods));
    assert(methods.count == 1 && methods.methods[0] == 0x544 && methods.values[0] == 0xff);
    assert(nvn_command_gap_no_writes(clip_packet, 2));
    const u32 vertex_range[] = {0x20020701, 6, 0x22ad0000, 0x200207c0, 6, 0x22ad0d8b};
    assert(nvn_command_gap_no_writes(vertex_range, 6));
    assert(!nvn_command_gap_no_writes(packet, 12)); /* Indexed draw writes attachments. */
    const u32 clear_packet[] = {0x60010674, 1};
    const u32 upload_packet[] = {0x200408e1, 6, 0x22ad0000, 0, 0x12345678};
    const u32 report_packet[] = {0x200406c0, 6, 0x22ad0000, 1, 0};
    const u32 acquire_packet[] = {0x200406c0, 6, 0x22ad0000, 1, 1};
    const u32 counter_packet[] = {0x200406c0, 6, 0x22ad0000, 1, 2};
    const u32 puller_acquire[] = {0x20040004, 6, 0x22ad0000, 1, 1};
    const u32 puller_release[] = {0x20040004, 6, 0x22ad0000, 1, 2};
    const u32 puller_release_flush[] = {0x20040004, 5, 0x31ca660, 0, 0x01000002, 0x800003e0};
    const u32 upload_nop[] = {0x200408e1, 6, 0x22ad0000, 0, 0x12345678, 0x8000068b};
    const u32 nonzero_flush[] = {0x800103e0};
    const u32 nonzero_nop[] = {0x8001068b};
    const u32 other_subchannel[] = {0x60012904, 0x31};
    const u32 upload_before_draw[] = {0x200408e1, 6, 0x22ad0000, 0, 0x12345678, 0xa0050e38, 4, 6, 0x22ad0d90, 1, 1656};
    const u32 other_subchannel_before_draw[] = {0x60012904, 0x31, 0xa0050e38, 4, 6, 0x22ad0d90, 1, 1656};
    assert(!nvn_command_gap_modeled_indexed_draw(upload_before_draw, 11));
    assert(!nvn_command_gap_modeled_indexed_draw(other_subchannel_before_draw, 8));
    assert(!nvn_command_gap_modeled_indexed_draw(packet, 11));
    assert(nvn_command_gap_staged_writes(packet, 12, 1));
    assert(!nvn_command_gap_staged_writes(packet, 12, 0));
    assert(nvn_command_gap_staged_writes(upload_packet, 5, 0));
    assert(nvn_command_gap_staged_writes(report_packet, 5, 0));
    assert(nvn_command_gap_staged_writes(puller_release, 5, 0));
    /* puller methods act on every subchannel, as gpu.c decodes them: the same release on subchannel 1 */
    const u32 puller_release_sub1[] = {0x20042004, 6, 0x22ad0000, 1, 2};
    assert(nvn_command_gap_staged_writes(puller_release_sub1, 5, 0));
    assert(!nvn_command_gap_no_writes(puller_release_sub1, 5));
    assert(nvn_command_gap_staged_writes(puller_release_flush, 6, 0));
    assert(nvn_command_gap_staged_writes(upload_nop, 6, 0));
    assert(nvn_command_gap_no_writes(puller_release_flush + 5, 1));
    assert(nvn_command_gap_no_writes(upload_nop + 5, 1));
    assert(!nvn_command_gap_no_writes(nonzero_flush, 1));
    assert(!nvn_command_gap_no_writes(nonzero_nop, 1));
    assert(!nvn_command_gap_staged_writes(nonzero_flush, 1, 0));
    assert(!nvn_command_gap_staged_writes(nonzero_nop, 1, 0));
    assert(nvn_command_gap_staged_writes(upload_before_draw, 11, 1));
    assert(!nvn_command_gap_staged_writes(other_subchannel_before_draw, 8, 1));
    assert(!nvn_command_gap_staged_writes(other_subchannel, 2, 0));
    assert(!nvn_command_gap_staged_writes(packet, 11, 1));
    assert(!nvn_command_gap_no_writes(clear_packet, 2));
    assert(!nvn_command_gap_no_writes(upload_packet, 5)); /* CBUF data method writes guest memory. */
    assert(!nvn_command_gap_no_writes(report_packet, 5)); /* Query release writes guest memory. */
    assert(nvn_command_gap_no_writes(acquire_packet, 5)); /* Query acquire reads the semaphore. */
    assert(!nvn_command_gap_no_writes(counter_packet, 5));
    assert(nvn_command_gap_no_writes(puller_acquire, 5));
    assert(!nvn_command_gap_no_writes(puller_release, 5));
    NvnGapMemoryState report_state = {0};
    NvnGapMemoryWrite writes[2] = {0};
    u32 write_count = 99;
    assert(!nvn_command_gap_memory_writes(report_packet, 5, &report_state, writes, 0, &write_count));
    assert(write_count == 99 && !report_state.query_known);
    assert(nvn_command_gap_memory_writes(report_packet, 5, &report_state, writes, 2, &write_count));
    assert(write_count == 1 && writes[0].kind == NVN_GAP_WRITE_QUERY && writes[0].address == 0x622ad0000ull &&
           writes[0].size == 16 && writes[0].value_known && writes[0].value == 1 && writes[0].fake_known &&
           !writes[0].fake_report);
    assert(nvn_command_gap_memory_writes(acquire_packet, 5, &report_state, writes, 2, &write_count));
    assert(write_count == 0);
    const u32 puller_address[] = {0x20030004, 6, 0x22ad0000, 1};
    const u32 puller_word_release[] = {0x60010007, 0x01000002};
    assert(nvn_command_gap_memory_writes(puller_address, 4, &report_state, writes, 2, &write_count));
    assert(write_count == 0 && report_state.puller_known == 7 && report_state.puller_payload == 1);
    assert(nvn_command_gap_memory_writes(puller_word_release, 2, &report_state, writes, 2, &write_count));
    assert(write_count == 1 && writes[0].kind == NVN_GAP_WRITE_PULLER && writes[0].address == 0x622ad0000ull &&
           writes[0].size == 4 && writes[0].value_known && writes[0].value == 1);
    const u32 puller_zero_release[] = {0x60010006, 0, 0x60010007, 2};
    assert(nvn_command_gap_memory_writes(puller_zero_release, 4, &report_state, writes, 2, &write_count));
    assert(write_count == 1 && writes[0].kind == NVN_GAP_WRITE_PULLER && writes[0].size == 16 &&
           writes[0].value_known && writes[0].value == 0);
    assert(nvn_command_gap_memory_writes(counter_packet, 5, &report_state, writes, 2, &write_count));
    assert(write_count == 1 && writes[0].kind == NVN_GAP_WRITE_QUERY && writes[0].size == 16 && writes[0].value_known &&
           writes[0].value == 0 && !writes[0].fake_report);
    const u32 samples_counter[] = {0x600106c3, 0x00800002};
    assert(nvn_command_gap_memory_writes(samples_counter, 2, &report_state, writes, 2, &write_count));
    assert(write_count == 1 && writes[0].kind == NVN_GAP_WRITE_QUERY && writes[0].fake_known && writes[0].fake_report &&
           !writes[0].value_known);
    const u32 two_releases[] = {0x60010007, 2, 0x60010007, 2};
    NvnGapMemoryState before_overflow = report_state;
    assert(!nvn_command_gap_memory_writes(two_releases, 4, &report_state, writes, 1, &write_count));
    assert(!memcmp(&report_state, &before_overflow, sizeof report_state));
    const u32 cbuf_words[] = {0x200308e1, 6,          0x22ad0000, 0x20,      0x600408e4,
                              0x11111111, 0x22222222, 0x33333333, 0x44444444};
    assert(nvn_command_gap_memory_writes(cbuf_words, 9, &report_state, writes, 2, &write_count));
    assert(write_count == 1 && writes[0].kind == NVN_GAP_WRITE_CBUF && writes[0].address == 0x622ad0020ull &&
           writes[0].size == 16);
    assert(report_state.cbuf_offset == 0x30);
    const u32 cbuf_then_report[] = {0x600108e4, 0x55555555, 0x60010007, 2};
    assert(nvn_command_gap_memory_writes(cbuf_then_report, 4, &report_state, writes, 2, &write_count));
    assert(write_count == 2 && writes[0].kind == NVN_GAP_WRITE_CBUF && writes[0].address == 0x622ad0030ull &&
           writes[1].kind == NVN_GAP_WRITE_PULLER);
    const u32 macro_then_data[] = {0x60010e20, 1, 0x600108e4, 0xdeadbeef};
    before_overflow = report_state;
    assert(!nvn_command_gap_memory_writes(macro_then_data, 4, &report_state, writes, 2, &write_count));
    assert(!memcmp(&report_state, &before_overflow, sizeof report_state));
    NvnGapMemoryState wrapped_address = {
        .cbuf_high = UINT32_MAX, .cbuf_low = UINT32_MAX, .cbuf_offset = 1, .cbuf_known = 7};
    const u32 one_data_word[] = {0x600108e4, 1};
    assert(!nvn_command_gap_memory_writes(one_data_word, 2, &wrapped_address, writes, 2, &write_count));
    assert(wrapped_address.cbuf_offset == 1);
    NvnGapMemoryState wrapped_offset = {.cbuf_high = 0, .cbuf_low = 0, .cbuf_offset = UINT32_MAX - 3, .cbuf_known = 7};
    assert(!nvn_command_gap_memory_writes(one_data_word, 2, &wrapped_offset, writes, 2, &write_count));
    assert(wrapped_offset.cbuf_offset == UINT32_MAX - 3);
    assert(!nvn_command_gap_no_writes(other_subchannel, 2));
    assert(!nvn_command_gap_no_writes(binding_packets, 7));

    u32 altered[12];
    memcpy(altered, packet, sizeof altered);
    altered[11] = 3384;
    assert(nvn_command_gap_indexed_draw(altered, 12, &draw) && draw.count == 3384);
    memcpy(altered, packet, sizeof altered);
    altered[6] = 0xa0050e18; /* another MME method is not an indexed draw */
    assert(!nvn_command_gap_indexed_draw(altered, 12, &draw));
    memcpy(altered, packet, sizeof altered);
    altered[10] = 3; /* unsupported index width */
    assert(!nvn_command_gap_indexed_draw(altered, 12, &draw));
    memcpy(altered, packet, sizeof altered);
    altered[0] = 0x200d0701; /* a malformed prefix cannot hide a draw packet */
    assert(!nvn_command_gap_indexed_draw(altered, 12, &draw));
    methods.count = 0;
    assert(!nvn_command_gap_methods(altered, 12, capture, &methods) && !methods.count);
    assert(!nvn_command_gap_indexed_draw(packet, 11, &draw));
    assert(!nvn_command_gap_indexed_draw(NULL, 12, &draw));

    /* TotK's descriptor upload: the MME argument is a word offset, but that interpretation belongs
       to its uploaded macro program and is not inferred by this packet parser. */
    const u32 descriptor_packet[] = {
        0x60010e0e, 4,          0x6001068b, 0,          0x600108e3, 0xa8, 0x600108e4,
        0x14d0103c, 0x60010e20, 0x154,      0x200308e4, 0x23130000, 6,    0xe5e0,
    };
    assert(!nvn_command_gap_no_writes(descriptor_packet, 14)); /* CBUF data upload writes memory. */
    NvnGapMacroData3 descriptor = {0};
    assert(nvn_command_gap_macro_data3(descriptor_packet, 14, 0xe20, &descriptor));
    assert(descriptor.macro_argument == 0x154);
    assert(descriptor.words[0] == 0x23130000 && descriptor.words[1] == 6 && descriptor.words[2] == 0xe5e0);
    assert(!nvn_command_gap_macro_data3(descriptor_packet, 14, 0xe18, &descriptor));
    u32 interrupted[14];
    memcpy(interrupted, descriptor_packet, sizeof interrupted);
    interrupted[10] = 0x200308e3; /* An offset setter in place of the first data word breaks the sequence. */
    assert(!nvn_command_gap_macro_data3(interrupted, 14, 0xe20, &descriptor));
    assert(!nvn_command_gap_macro_data3(descriptor_packet, 13, 0xe20, &descriptor));
    return 0;
}
