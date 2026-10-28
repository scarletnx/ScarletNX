/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Deterministic queue scheduling: real release/writeback code, fake completed GPU
   submissions. No Vulkan device or worker timing is needed to expose recursion,
   decreasing gates, physical aliases, and remapping order. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/runtime/platform/plat.h"
#define SNX_VK_H
#define VK_NULL_HANDLE 0
void vk_result_begin(void);
void vk_result_end(void);
int vk_result_landing(void);
void vk_lock(void);
void vk_unlock(void);
void vk_release_events(void);
void vk_compute_land_through(u64);
void vk_land_through(u64);
void vk_wait_submit(u64);
void vk_compute_wait(u64);
void vk_gfx_image_land(void);
void vk_gfx_image_put_aside(void);
void vk_rt_publish_linear(void);
int vk_gfx_image_in_flight(int unsubmitted);
u64 vk_compute_put_aside(void);
int vk_buf_pending_any(void);
int vk_buf_pending(u64, u64);
int vk_compute_pending(u64, u64);
int vk_gfx_image_pending(u64, u64);
int vk_buf_pending_unsubmitted(void);
void vk_submit(u64, u64, u64);
u64 vk_last_submit(void);
u64 vk_submit_number(void);
void vk_rt_before_write(u64, u64);
u64 vk_query_value(u32, u64, u64);
#define VK_PRED_EQUAL 0x80000000u
#define VK_QUERY_COUNTERS 8
enum { VK_PRED_PREV_NONE, VK_PRED_PREV_ZERO, VK_PRED_PREV_NONZERO, VK_PRED_PREV_SLOT };
int vk_query_pred_ok(void);
int vk_query_pred_job(u32, u64, u64, u64, int, u32, u32*);
int vk_query_pred_flush(void);
void vk_query_pred_hold(u32);
void vk_query_pred_drop(u32);
int vk_query_pred_compare(u64, u32*);
int vk_render_predicate(u64 a, int equal, u32* pred);
void vk_render_predicate_drop(u32 pred);
#include "nx/gpu/vk/release.c"
typedef void (*VkChangedRun)(void* user, u32 from, u32 to); /* vk.h's, for writeback.c */
#include "nx/gpu/vk/writeback.c"

int g_gpu_releases;
static unsigned lock_depth, landing_calls, payload_writes, submits, query_reads, events_signaled;
static int events_due;
static u64 next_compute, next_graphics = 1, waited_compute, waited_graphics;
static int pending, unsubmitted, landing_probe, other_pending;
static u32 completed[128];
static u8 memory[0x8000];
/* the guest address space is that memory: vk_write_changed refuses to compare bytes inside it */
u8* g_mem = memory;
u64 g_as_size = sizeof memory;
static u64 mapping = 4096;
static int writeback_direct;
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 n) {
    (void)c;
    (void)m;
    (void)n;
    abort();
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    (void)fn;
    (void)arg;
    (void)stack;
    return 0;
}
void fatal(const char* fmt, ...) {
    (void)fmt;
    abort();
}
void logf_(int level, const char* fmt, ...) {
    (void)level;
    (void)fmt;
}
void vk_lock(void) { lock_depth++; }
/* as device.c's: the outermost unlock signals the event slots the releases under it reached */
void vk_unlock(void) {
    assert(lock_depth);
    if (!--lock_depth) vk_release_events();
}
u64 gmmu_translate(u64 address) {
    if (address >= 0x10000 && address < 0x11000) return mapping + (address & 4095);
    if (address >= 0x20000 && address < 0x21000) return 4096 + (address & 4095);
    if (address >= 0x30004 && address < 0x38000) return address - 0x30000; /* all of memory, for the index's homes */
    return 0;
}
/* the queued payloads' page index (pending_pages.c) translates runs of contiguous pages */
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    u64 n, first = gmmu_translate(gva);
    if (!max || !first) return 0;
    *cpu = first;
    n = 0x1000 - (gva & 0xFFF);
    while (n < max && gmmu_translate(gva + n) == first + n) n += 0x1000;
    return n < max ? n : max;
}
/* nvdrv.c's index of mappings by guest address, by brute force over the pages above (the middle of each, as the last
   range starts a few bytes in): each GPU page showing guest bytes of [cpu, cpu + size), clipped to them */
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx) {
    for (u64 p = 0; p < 0x40; p++) {
        u64 c = gmmu_translate((p << 12) + 0x800) & ~0xFFFull, a = c > cpu ? c : cpu;
        u64 b = c + 0x1000 < cpu + size ? c + 0x1000 : cpu + size;
        if (gmmu_translate((p << 12) + 0x800) && a < b) fn(ctx, (p << 12) + (a - c), b - a);
    }
}
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx) {
    for (u64 p = gva >> 12; p <= (gva + size - 1) >> 12 && p < 0x40; p++)
        if (gmmu_translate((p << 12) + 0x800)) fn(ctx, p << 12, gmmu_translate((p << 12) + 0x800) & ~0xFFFull, 0x1000);
}
u32 syncpt_read(u32 id) { return id < 128 ? completed[id] : 0; }
/* under vk_lock, so the events it reaches wait for vk_unlock (memory/own.c's traps are answered under vk_lock) */
void syncpt_add_quiet(u32 id, u32 n) {
    assert(lock_depth && vk_result_landing());
    completed[id] += n;
    events_due = 1;
}
/* nvdrv.c's: signals with no vk_lock held, what was due since the last call */
void syncpt_events(void) {
    assert(!lock_depth);
    if (!events_due) return;
    events_due = 0;
    events_signaled++;
}
int gmem_write(u64 address, const void* data, u64 size) {
    assert(lock_depth && vk_result_landing());
    vk_release_need(address, size); /* the real memory layer reenters here */
    u64 physical = gmmu_translate(address);
    assert(physical && physical + size <= sizeof memory);
    memcpy(memory + physical, data, size);
    payload_writes++;
    return 1;
}
u8* gmem_ptr(u64 address, u64 size) {
    (void)size;
    return writeback_direct ? memory + gmmu_translate(address) : NULL;
}
int gmem_read(u64 address, void* data, u64 size) {
    vk_release_need(address, size);
    memcpy(data, memory + gmmu_translate(address), size);
    return 1;
}
void gpu_mem_written_shader(u64 address, u64 size) {
    (void)address;
    (void)size;
}
void vk_rt_before_write(u64 address, u64 size) {
    (void)address;
    (void)size;
}
void vk_compute_land_through(u64 gate) {
    assert(lock_depth && vk_result_landing());
    assert(waited_compute >= gate);
    landing_calls++;
    if (landing_probe) {
        landing_probe = 0;
        u8 original = 0, result = 7;
        vk_write_changed(0x10000, &result, &original, 1);
        assert(completed[3] == 0); /* release follows all output reconciliation */
    }
}
void vk_land_through(u64 gate) { assert(waited_graphics >= gate); }
void vk_wait_submit(u64 gate) {
    if (gate > waited_graphics) waited_graphics = gate;
}
void vk_compute_wait(u64 gate) {
    if (gate > waited_compute) waited_compute = gate;
}
void vk_gfx_image_land(void) {}
void vk_gfx_image_put_aside(void) {}
void vk_rt_publish_linear(void) {}
int vk_gfx_image_in_flight(int unsubmitted) {
    (void)unsubmitted;
    return 0;
}
u64 vk_compute_put_aside(void) {
    u64 value = next_compute;
    next_compute = 0;
    return value;
}
int vk_buf_pending_any(void) { return pending; }
/* another storage result pending over the range read (vk_release_read waits for it instead) */
int vk_compute_pending(u64 a, u64 n) {
    (void)a;
    (void)n;
    return other_pending;
}
int vk_buf_pending(u64 a, u64 n) {
    (void)a;
    (void)n;
    return 0;
}
int vk_gfx_image_pending(u64 a, u64 n) {
    (void)a;
    (void)n;
    return 0;
}
void gmem_read_raw(u64 address, void* data, u64 size) { memcpy(data, memory + gmmu_translate(address), size); }
int vk_buf_pending_unsubmitted(void) { return unsubmitted; }
/* a submit: what was recorded becomes the last submission */
void vk_submit(u64 a, u64 b, u64 c) {
    (void)a;
    (void)b;
    (void)c;
    submits++;
    next_graphics++;
}
/* a made-up count of query.c's counter 3's segments, read under the lock when a report is performed */
u64 vk_query_value(u32 counter, u64 first, u64 end) {
    assert(lock_depth && !vk_result_landing() && counter == 3);
    query_reads++;
    return (end - first) * 100 + first;
}
/* render enable predicates (query.c): a report of segments [first, end) gets slot first when its count isn't known,
   else the count's zeroness comes from job_known */
static int job_known = 2;
static unsigned pred_flushes;
static u32 pred_refs[64];
int vk_query_pred_ok(void) { return 1; }
int vk_query_pred_job(u32 counter, u64 first, u64 end, u64 prev_end, int prev, u32 prev_slot, u32* slot) {
    (void)prev_end;
    (void)prev_slot;
    assert(lock_depth && counter == 3 && end > first && first < 64 && prev == VK_PRED_PREV_NONE);
    if (job_known != 2) return job_known;
    pred_refs[first] = 1;
    *slot = (u32)first;
    return 2;
}
int vk_query_pred_flush(void) {
    assert(lock_depth);
    pred_flushes++;
    return 1;
}
/* the GPU's comparison of two values a result not landed holds: slot 40 */
static unsigned pred_compares;
int vk_query_pred_compare(u64 a, u32* slot) {
    assert(lock_depth && a == 0x10180);
    pred_compares++;
    pred_refs[40] = 1;
    *slot = 40;
    return 1;
}
void vk_query_pred_hold(u32 slot) { pred_refs[slot]++; }
void vk_query_pred_drop(u32 slot) {
    assert(pred_refs[slot]);
    pred_refs[slot]--;
}
u64 vk_last_submit(void) { return next_graphics; }
u64 vk_submit_number(void) { return next_graphics + 1; }

/* Compare both mapping paths against the byte merge rule, including chunk
 * boundaries, dense writes, unchanged output and intervening CPU writes.
 */
/* the runs vk_write_changed_runs tells, collected, against buffer.c's former scan of its own: 8-byte granules from the
   result's start where it differs from the original, merged when they touch */
static u32 runs_seen[1024][2], nruns_seen;
static void collect_run(void *user, u32 from, u32 to)
{
	(void)user;
	assert(nruns_seen < 1024);
	runs_seen[nruns_seen][0] = from;
	runs_seen[nruns_seen][1] = to;
	nruns_seen++;
}
static void check_runs(const u8 *result, const u8 *original, u32 n)
{
	u32 want[1024][2], nwant = 0;

	for (u32 k = 0; k < n; k += 8) {
		u32 w = n - k < 8 ? n - k : 8;

		if (!memcmp(result + k, original + k, w))
			continue;
		if (nwant && want[nwant - 1][1] == k) {
			want[nwant - 1][1] = k + w;
		} else {
			want[nwant][0] = k;
			want[nwant][1] = k + w;
			nwant++;
		}
	}
	assert(nruns_seen == nwant && !memcmp(runs_seen, want, nwant * sizeof *want));
}

static void check_writeback_merge(void)
{
	u8 original[4099], result[4099], current[4099], expected[4099];
	u32 state = 0x734fd21u;
	const u32 lengths[] = {0, 1, 7, 8, 9, 1023, 1024, 1025, 2049, 4099};

	mapping = 4096;
	for (int direct = 0; direct < 2; direct++) {
		writeback_direct = direct;
		for (u32 pass = 0; pass < 32; pass++) {
			for (u32 i = 0; i < sizeof original; i++) {
				state = state * 1664525u + 1013904223u;
				original[i] = (u8)(state >> 24);
				result[i] = pass == 0 || (state & 3) ? original[i] : (u8)(original[i] ^ 0x5a);
				current[i] = state & 8 ? original[i] : (u8)(original[i] ^ 0xa5);
				if (pass == 1) result[i] = (u8)(original[i] ^ 0xff);
				expected[i] = result[i] != original[i] && current[i] == original[i]
					      ? result[i] : current[i];
			}
			for (u32 k = 0; k < sizeof lengths / sizeof *lengths; k++) {
				u32 n = lengths[k];

				memcpy(memory + mapping, current, sizeof current);
				vk_lock();
				vk_write_changed(0x31000, result, original, n);
				vk_unlock();
				assert(!memcmp(memory + mapping, expected, n));
				assert(!memcmp(memory + mapping + n, current + n, sizeof current - n));
				assert(!vk_result_landing());
				/* the same landing telling its runs */
				memcpy(memory + mapping, current, sizeof current);
				nruns_seen = 0;
				vk_lock();
				vk_write_changed_runs(0x31000, result, original, n, collect_run, NULL);
				vk_unlock();
				assert(!memcmp(memory + mapping, expected, n));
				assert(!memcmp(memory + mapping + n, current + n, sizeof current - n));
				check_runs(result, original, n);
			}
		}
	}
	writeback_direct = 0;
}

int main(void) {
    pending = 1;
    next_compute = 9;
    vk_release_incr(3);
    next_compute = 0; /* older compute work has landed through another read */
    u8 value = 11;
    vk_release_write(0x10000, &value, 1);
    assert(g_rq[1].cgate == 9 && vk_release_value(3) == 1);
    landing_probe = 1;
    vk_release_need(0x20000, 1); /* another GVA names the same backing */
    assert(landing_calls == 1 && completed[3] == 1 && memory[4096] == 11);
    assert(events_signaled == 1 && !events_due); /* the increment's events, at the outermost unlock */
    assert(g_rhead == g_rtail && !g_gpu_releases && !vk_result_landing());
    assert(vk_release_value(3) == 1);
    value = 13;
    vk_release_write(0x10000, &value, 1);
    vk_release_drain(); /* mapping callback must drain before changing backing */
    mapping = 1;
    assert(memory[4096] == 13 && memory[1] == 0);
    pending = 0;
    value = 17;
    /* with no work before it, the release waits only for the stream to stop (the commands after it read guest memory
       first), then goes at once, holding the mapping lock */
    vk_release_write(0x10000, &value, 1);
    assert(memory[1] == 0 && payload_writes == 3 && g_rhead == g_rtail + 1);
    vk_release_submit();
    assert(memory[1] == 17 && payload_writes == 4 && g_rhead == g_rtail);
    pending = 1;
    vk_release_incr(5);
    vk_release_retire(5);
    assert(completed[5] == 1 && g_rhead == g_rtail && events_signaled == 2 && !events_due);
    /* a samples-passed report waits for the submit being recorded even with nothing pending, without submitting it:
       whoever needs it submits, and its count is read when it is performed (the count, then the ticks) */
    pending = 0;
    unsigned before = submits;
    u64 recording = vk_submit_number();
    vk_release_samples(0x10010, 16, 3, 2, 5, 0x77);
    assert(submits == before && g_rhead == g_rtail + 1 && g_rq[g_rtail % RQ_N].gate == recording && !query_reads);
    vk_release_need(0x10010, 16);
    u64 report[2] = {snx_ld_le64(memory + 1 + 0x10), snx_ld_le64(memory + 1 + 0x18)}; /* guest memory's order */
    assert(submits == before + 1 && vk_last_submit() == recording);
    assert(query_reads == 1 && report[0] == 302 && report[1] == 0x77 && g_rhead == g_rtail);
    /* the end of the GPFIFO submission submits it too, and a second stream end has nothing to submit; the count's low
       word alone */
    vk_release_samples(0x10030, 4, 3, 7, 9, 0x99);
    vk_release_submit();
    vk_release_submit();
    assert(submits == before + 2);
    vk_release_drain();
    u32 word = snx_ld_le32(memory + 1 + 0x30);
    assert(query_reads == 2 && word == 207 && memory[1 + 0x34] == 0);
    /* a release after storage results in the submit being recorded waits for that submit the same way, without
       submitting it: the end of the GPFIFO submission does */
    pending = unsubmitted = 1;
    before = submits;
    recording = vk_submit_number();
    vk_release_incr(6);
    assert(submits == before && g_rq[(g_rhead - 1) % RQ_N].gate == recording && completed[6] == 0);
    vk_release_submit();
    assert(submits == before + 1 && vk_last_submit() == recording);
    unsubmitted = 0;
    vk_release_retire(6);
    assert(completed[6] == 1 && g_rhead == g_rtail && events_signaled == 3 && !events_due);
    pending = 0;
    /* a read over queued releases takes their payloads, in their order, without waiting for the work before them or
       writing memory; the same memory through another address, another pending result there or a samples-passed count
       not known yet make it decline, and the caller waits as before */
    mapping = 4096; /* 0x10000 and 0x20000 name the same page */
    pending = 1;
    u64 waited = waited_graphics;
    value = 21;
    vk_release_write(0x10040, &value, 1);
    value = 23;
    vk_release_write(0x10041, &value, 1);
    value = 25;
    vk_release_write(0x10040, &value, 1);
    memory[4096 + 0x42] = 9;
    u8 got[3] = {0};
    assert(vk_release_read(0x10040, got, 3) == 1 && got[0] == 25 && got[1] == 23 && got[2] == 9);
    assert(waited_graphics == waited && memory[4096 + 0x40] == 0 && g_rhead == g_rtail + 3 &&
           g_vk_release_forwards == 1);
    assert(vk_release_read(0x20040, got, 1) == -1);
    other_pending = 1;
    assert(vk_release_read(0x10040, got, 1) == -1 && vk_release_read(0x10060, got, 1) == 0);
    other_pending = 0;
    vk_release_samples(0x10048, 4, 3, 1, 2, 0);
    assert(vk_release_read(0x10048, got, 4) == -1 && vk_release_read(0x10050, got, 1) == 0);
    vk_release_drain();
    assert(memory[4096 + 0x40] == 25 && memory[4096 + 0x41] == 23 && g_rhead == g_rtail);
    /* the render enable's comparison of a count not known yet with a zero 16 bytes on: a predicate for each queued
       report, made in one batch, held by the report until it is performed and by the channel until it lets go; IF_EQUAL
       inverts it. A nonzero other side, or a report not the last over the count, is compared by reading, as before */
    u32 pred = 0;
    memset(memory + 4096 + 0x100, 0, 0x40);
    vk_release_samples(0x10100, 16, 3, 10, 11, 0);
    vk_release_samples(0x10120, 16, 3, 11, 12, 0);
    assert(vk_render_predicate(0x10100, 0, &pred) == 2 && pred == 11 && pred_flushes == 1);
    assert(pred_refs[10] == 2 && pred_refs[11] == 1 && g_rhead == g_rtail + 2);
    assert(vk_render_predicate(0x10120, 1, &pred) == 2 && pred == (12 | VK_PRED_EQUAL) && pred_flushes == 1);
    vk_render_predicate_drop(pred);
    assert(pred_refs[11] == 1);
    memory[4096 + 0x130] = 1; /* the other side isn't zero */
    assert(vk_render_predicate(0x10120, 0, &pred) == -1);
    memory[4096 + 0x130] = 0;
    value = 3; /* a later write over the count */
    vk_release_write(0x10104, &value, 1);
    assert(vk_render_predicate(0x10100, 0, &pred) == -1);
    vk_release_drain();
    assert(pred_refs[10] == 1 && pred_refs[11] == 0 && g_rhead == g_rtail); /* the channel still holds slot 10 */
    vk_render_predicate_drop(11);
    assert(pred_refs[10] == 0);
    /* values a result not landed holds, no release queued over them: compared on the GPU, not read (which would wait
       for the result to land); with nothing pending they are read as before */
    other_pending = 1;
    assert(vk_render_predicate(0x10180, 1, &pred) == 2 && pred == (41 | VK_PRED_EQUAL) && pred_compares == 1);
    vk_render_predicate_drop(pred);
    assert(!pred_refs[40]);
    other_pending = 0;
    assert(vk_render_predicate(0x10180, 1, &pred) == -1 && pred_compares == 1);
    job_known = 0; /* a count known to be zero: IF_EQUAL draws, IF_NOT_EQUAL doesn't, without a predicate */
    vk_release_samples(0x10140, 16, 3, 13, 14, 0);
    assert(vk_render_predicate(0x10140, 1, &pred) == 1 && vk_render_predicate(0x10140, 0, &pred) == 0);
    vk_release_drain();
    /* two words of one home in the payloads' index (guest 0x1100 and 0x7ab4): the second goes in the slot after it,
       and moves back to its home when the older is performed, still found there */
    u32 word4 = 5;
    vk_release_write(0x10100, &word4, 4);
    u64 seq_a = g_rhead - 1;
    vk_submit(0, 0, 0); /* the second waits for a later submit, so the first is performed alone */
    vk_release_write(0x37ab4, &word4, 4);
    u64 seq_b = g_rhead - 1;
    u32 home = rix_home(0x1100, RIX_N - 1);
    assert(home == rix_home(0x7ab4, RIX_N - 1) && !g_rodd);
    assert(g_rix[home].key == 0x1100 && g_rix[home].seq == seq_a && g_rix[(home + 1) & (RIX_N - 1)].key == 0x7ab4);
    assert(count_queued(0x10100) == -2 && count_queued(0x37ab4) == -2); /* queued, though not samples counts */
    vk_release_need(0x10100, 4);
    assert(g_rtail == seq_b && g_rix[home].key == 0x7ab4 && g_rix[home].seq == seq_b && g_rix[home].n == 1);
    assert(!g_rix[(home + 1) & (RIX_N - 1)].key && count_queued(0x10100) == -1 && count_queued(0x37ab4) == -2);
    /* the slot it left holds no count: a word of that home (guest 0x5254) queued there goes when it is performed */
    assert(rix_home(0x5254, RIX_N - 1) == ((home + 1) & (RIX_N - 1)) && !g_rix[(home + 1) & (RIX_N - 1)].n);
    vk_release_write(0x35254, &word4, 4);
    assert(g_rix[(home + 1) & (RIX_N - 1)].key == 0x5254 && g_rix[(home + 1) & (RIX_N - 1)].n == 1);
    vk_release_drain();
    assert(!g_rix[home].key && !g_rix[(home + 1) & (RIX_N - 1)].key && count_queued(0x37ab4) == -1);
    /* a range finds the latest release over it by its pages (latest_over), through the words' index or the page's
       releases: of three payloads in one page each queued after a submit, a read of the oldest performs it alone, one
       over the page's other words none, and one over the newest the two left; with one queued in another page, which a
       range over both performs */
    u64 seq0 = g_rhead;
    for (u32 i = 0; i < 3; i++) {
        word4 = 30 + i;
        vk_submit(0, 0, 0);
        vk_release_write(0x10200 + 0x100 * i, &word4, 4);
    }
    vk_submit(0, 0, 0);
    vk_release_write(0x37000, &word4, 4);
    assert(g_rhead == seq0 + 4 && !g_rodd && g_rpg[rix_find(g_rpg, RPG_N - 1, 4096)].n == 3);
    vk_release_need(0x10200, 4);
    assert(g_rtail == seq0 + 1 && snx_ld_le32(memory + 4096 + 0x200) == 30 && snx_ld_le32(memory + 4096 + 0x300) == 0);
    vk_release_need(0x10500, 0x100); /* by the page's releases (more words than releases) */
    vk_release_need(0x10304, 0xfc);
    vk_release_need(0x10304, 4); /* by the words' index */
    assert(g_rtail == seq0 + 1);
    vk_release_need(0x10380, 0x100);
    assert(g_rtail == seq0 + 3 && snx_ld_le32(memory + 4096 + 0x400) == 32 && !g_rpg[rix_find(g_rpg, RPG_N - 1, 4096)].key);
    vk_submit(0, 0, 0);
    for (u32 i = 0; i < 3; i++) vk_release_write(0x10200 + 0x100 * i, &word4, 4); /* the page path again */
    vk_release_need(0x36ffc, 8);
    assert(g_rtail == seq0 + 4 && snx_ld_le32(memory + 0x7000) == 32);
    vk_release_drain();
    assert(!g_rpg[rix_find(g_rpg, RPG_N - 1, 4096)].key && !g_rpg[rix_find(g_rpg, RPG_N - 1, 0x7000)].key);
    assert(lock_depth == 0 && events_signaled == 3 && !events_due); /* payload writes signal nothing */
	check_writeback_merge();
    return 0;
}
