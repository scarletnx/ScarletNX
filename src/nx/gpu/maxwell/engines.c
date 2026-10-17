/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the 3D engine's methods: registers, macro calls, draws (merging instanced ones), constant buffer data, report
   semaphores. maxwell3d.c passes the work to the renderer */
#include "nx/gpu/maxwell/engines_int.h"

/* ============================================================ Maxwell 3D */
#define R3(i) (ch->r3d[i])
/* instancing comes in as N BEGIN/END pairs with the next-instance bit. we accumulate them and emit
   one draw of N instances once something else shows up */
static struct {
    int on, indexed;
    GpuChannel* ch;
    u32 topo, first, count, bv, bi, inst, first_instance, small_kind;
} g_pd;
void r3d_pending_flush(void) {
    if (!g_pd.on) return;
    g_pd.on = 0;
    r3d_draw(g_pd.ch, g_pd.indexed, g_pd.topo, g_pd.first, g_pd.count, g_pd.bv, g_pd.bi + g_pd.first_instance,
             g_pd.inst);
    if (g_pd.indexed == 3) g_pd.ch->inline_n = 0;
}
int r3d_pending_on(void) { return g_pd.on; }

/* timestamps for report semaphores, in the 614.4 MHz GPU clock (32 times the 19.2 MHz system tick).
   sead::GraphicsNvn::convertGPUTimeStampToSystemTick compares them against the CPU tick, so they use the same base
   (guest_ns): the guest time when the stream reaches the report. Games with DRS (SMO via al::GpuPerf /
   agl::perf::GPUTimeStamp) time the GPU with these. */
u64 gpu_ticks(void) {
    static u64 last;
    u64 n = guest_ns();
    if (n <= last) n = last + 16;
    last = n; /* monotonic: 16 ns on, so the tick (ns * 384 / 625) moves too */
    return n * 384 / 625;
}
/* the report addresses a counter wrote a made-up value to (every counter but ZERO is an estimate or a constant,
   samples passed included): a render enable condition over them would compare made-up counts and could skip a draw
   the game needs, so it draws, as it did before conditions were read. A release there, a real value, clears it */
#define FAKE_REPORTS 64
static u64 g_fake_report[FAKE_REPORTS];
static u32 g_fake_next;
static void fake_report_mark(u64 addr, int fake) {
    for (u32 i = 0; i < FAKE_REPORTS; i++)
        if (g_fake_report[i] == addr) {
            if (!fake) g_fake_report[i] = 0;
            return;
        }
    if (fake && addr) g_fake_report[g_fake_next++ % FAKE_REPORTS] = addr;
}
static int fake_report(u64 addr) {
    for (u32 i = 0; addr && i < FAKE_REPORTS; i++)
        if (g_fake_report[i] == addr) return 1;
    return 0;
}

void query_get(GpuChannel* ch, u32* r, int base, u32 v) {
    u64 addr = ((u64)(r[base] & 0xFF) << 32) | r[base + 1];
    u32 seq = r[base + 2];
    u32 op = v & 3;
    if (op == 0) { /* release */
        fake_report_mark(addr, 0);
        if (((v >> 4) & 1) && !((v >> 28) & 1)) { /* the size is bit 28: does bit 4 alone mean one word? */
            LOG_LIMIT(4, "gpu: report release %x at %llx with bit 4 set and bit 28 clear, written as one word\n", v,
                      (unsigned long long)addr);
        }
        if (((v >> 4) & 1) || ((v >> 28) & 1))
            gpu_release_write(addr, &seq, 4);
        else {
            u64 q[2] = {seq, gpu_ticks()};
            gpu_release_write(addr, q, 16);
        }
    } else if (op == 1) {
        /* acquire: COMPARISON (bit 16) is EQ (0) or GE (1). From a macro, the wait starts once the macro has run */
        gpu_park(ch, GPU_PARK_REPORT, addr, seq, (v >> 16) & 1);
    } else if (op == 2) { /* counter */
        u32 sel = (v >> 23) & 31;
        u64 val = sel == 0 ? 0 : (sel == 1 ? g_draws * 3 : 1000);
        if (ch && (ch->r3d_counter_reset_mask & (1u << sel))) {
            u64 draws = g_draws - ch->r3d_counter_base[sel];
            val = draws * (sel == 1 ? 3 : 1000);
        }
        fake_report_mark(addr, sel != 0);
        if ((v >> 28) & 1) {
            u32 w32 = (u32)val;
            gpu_release_write(addr, &w32, 4);
        } else {
            u64 q[2] = {val, gpu_ticks()};
            gpu_release_write(addr, q, 16);
        }
    }
}

/* SetRenderEnable, decided when its mode or the override is written, as Eden's ProcessQueryCondition does. The
   override (0x651: 1 always, 2 never) wins; IF_EQUAL and IF_NOT_EQUAL compare the 64 bit words at the address and
   16 bytes past it, the values of a pair of counter reports; CONDITIONAL, whose bytes aren't settled, draws when
   both words at the address are nonzero, as Eden reads it. Over made-up counter values (fake_report) it draws */
static int render_enabled(const GpuChannel* ch) {
    const u32* r = ch->r3d;
    u32 ov = r[0x651] & 3;
    if (ov == 1) return 1;
    if (ov == 2) return 0;
    u64 addr = ((u64)(r[0x554] & 0xFF) << 32) | r[0x555];
    u32 w[6] = {0};
    switch (r[0x556] & 7) {
        case 0: return 0; /* FALSE */
        case 1: return 1; /* TRUE */
        case 2:
            if (fake_report(addr)) return 1;
            gmem_read(addr, w, 8);
            return w[0] != 0 && w[1] != 0;
        case 3:
        case 4:
            if (fake_report(addr) || fake_report(addr + 16)) return 1;
            gmem_read(addr, w, 24);
            return (r[0x556] & 7) == 3 ? w[0] == w[4] && w[1] == w[5] : w[0] != w[4] || w[1] != w[5];
        default: LOG_LIMIT(4, "gpu: render enable mode %u isn't known, drawing\n", r[0x556] & 7); return 1;
    }
}

static void inline_index_push(GpuChannel* ch, u32 v) {
    if (ch->inline_n == ch->inline_cap) {
        u32 cap = ch->inline_cap ? ch->inline_cap * 2 : 64;
        if (cap > (1u << 24)) {
            WARN_LIMIT(1, "gpu: inline index buffer exceeds 16M indices (remaining indices skipped)\n");
            return;
        }
        u32* p = realloc(ch->inline_indices, (size_t)cap * sizeof *p);
        if (!p) {
            WARN_LIMIT(1, "gpu: no memory for inline indices (remaining indices skipped)\n");
            return;
        }
        ch->inline_indices = p;
        ch->inline_cap = cap;
    }
    ch->inline_indices[ch->inline_n++] = v;
}

static void counter_reset(GpuChannel* ch, u32 reset) {
    /* Report selectors and reset selectors use different enums. Keep these estimates relative to the reset rather
       than returning the pre-reset constant for a counter that the game just cleared. */
    u32 mask;
    switch (reset) {
        case 0x01: mask = 1u << 0x15; break;                                                /* samples passed */
        case 0x02: mask = (1u << 0x0A) | (1u << 0x0C) | (1u << 0x0E) | (1u << 0x10); break; /* ZCull stats */
        case 0x10: mask = 1u << 0x0B; break;                  /* transform feedback primitives */
        case 0x12: mask = 1u << 0x01; break;                  /* input/generated vertices */
        case 0x13: mask = (1u << 0x03) | (1u << 0x12); break; /* input primitives; Eden: generated primitives */
        case 0x15: mask = 1u << 0x05; break;                  /* vertex shader invocations */
        case 0x16: mask = 1u << 0x1B; break;                  /* tess control invocations */
        case 0x17: mask = 1u << 0x1D; break;                  /* tess evaluation invocations */
        case 0x18: mask = 1u << 0x1F; break;                  /* tess evaluation primitives */
        case 0x1A: mask = 1u << 0x07; break;                  /* geometry shader invocations */
        case 0x1B: mask = 1u << 0x09; break;                  /* geometry shader primitives */
        case 0x1C: mask = 1u << 0x0F; break;                  /* clipper input primitives */
        case 0x1D: mask = 1u << 0x11; break;                  /* clipper output primitives */
        case 0x1E: mask = 1u << 0x13; break;                  /* fragment shader invocations */
        case 0x1F: mask = 1u << 0x12; break;                  /* Ryujinx: generated primitives; Eden: VTG output */
        default: return;
    }
    for (u32 sel = 0; sel < 32; sel++)
        if (mask & (1u << sel)) ch->r3d_counter_base[sel] = g_draws;
    ch->r3d_counter_reset_mask |= mask;
}

void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last) {
    if (ch->cb_n && !(m >= 0x8E4 && m <= 0x8F3)) r3d_const_flush(ch); /* constant buffer data lands first */
    if (m >= 0xE00) {                                                 /* macro call */
        if (!ch->mme_active) {
            ch->mme_active = 1;
            ch->mme_index = (m - 0xE00) / 2;
            ch->mme_nparams = 0;
        }
        if (ch->mme_nparams < 0x10000) ch->mme_params[ch->mme_nparams++] = v;
        if (last) {
            ch->mme_active = 0;
            mme_run(ch, ch->mme_index, ch->mme_nparams);
            ch->mme_nparams = 0;
        }
        return;
    }
    m &= 0xFFF;
    u32 raw = v;
    /* Puller/macro-control methods below 0x80 and WINDOW_ORIGIN (0x4EB) bypass shadow replay. */
    if (m >= 0x80) {
        if (ch->r3d_shadow_mode <= 1) {
            ch->r3d_shadow[m] = v;
            ch->r3d_shadow_valid[m] = 1;
        } else if (ch->r3d_shadow_mode == 3 && ch->r3d_shadow_valid[m] && m != 0x4EB)
            v = ch->r3d_shadow[m];
    }
    if (g_pd.on) { /* keep accumulating only for BEGIN(next instance), END, ranges, or writes that don't change a reg */
        int keep;
        switch (m) {
            case 0x586: keep = (v >> 26) & 1; break;
            case 0x486: keep = 1; break; /* VERTEX_ARRAY_INSTANCE_SUBSEQUENT: decides below */
            case 0x585:
            case 0x35D:
            case 0x35E:
            case 0x5F7:
            case 0x5F8:
            case 0x5FC:
            case 0x5FD:
            case 0x5FE: keep = 1; break;
            case 0x4C1:
            case 0x57A:
            case 0x57C: keep = 0; break; /* the next inline indices cannot modify a pending draw */
            case 0x45:
            case 0x46:
            case 0x47:
            case 0x48:
            case 0x6C:
            case 0x6D:
            case 0xB2:
            case 0x8C4:
            case 0x44:
            case 0x378:
            case 0x3DD:
            case 0x3DF:
            case 0x509:
            case 0x50A:
            case 0x54C:
            case 0x42B: /* DRAW_TEXTURE acts even when its argument repeats. */
            case 0x556: /* A repeated condition still rereads guest memory. */
            case 0x651:
            case 0x674:
            case 0x6C3:
            case 0x904:
            case 0x90C:
            case 0x914:
            case 0x91C:
            case 0x924: keep = 0; break;
            default: keep = (m < 0x8E4 || m > 0x8F3) && R3(m) == v; break;
        }
        if (!keep || g_pd.ch != ch) r3d_pending_flush();
    }
    R3(m) = v;
    switch (m) {
        case 0x49: ch->r3d_shadow_mode = raw & 3; break;
        case 0x54C: counter_reset(ch, v); break;
        case 0x44: /* WAIT_FOR_IDLE */
            gpu_signal_point();
            if (g_renderer_up && g_renderer->method_barrier) g_renderer->method_barrier(4);
            break;
        case 0x378: /* FRAGMENT_BARRIER */
        case 0x3DF: /* TILED_CACHE_BARRIER */
            if (g_renderer_up && g_renderer->method_barrier) g_renderer->method_barrier(m == 0x378 ? 0 : 1);
            break;
        case 0x3DD: /* INVALIDATE_TEXTURE_DATA_CACHE */
        case 0x509: /* INVALIDATE_SAMPLER_CACHE_NO_WFI */
        case 0x50A: /* INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI */
            if (m == 0x3DD) gpu_signal_point();
            if (g_renderer_up && g_renderer->method_barrier) g_renderer->method_barrier(m == 0x3DD ? 2 : 3);
            break;
        case 0x45: ch->mme_upload_ptr = v; break;
        case 0x46: ch->mme_code[ch->mme_upload_ptr++ & 0x1FFF] = v; break;
        case 0x47: ch->mme_start_ptr = v; break;
        case 0x48: ch->mme_start[ch->mme_start_ptr++ & 0xFF] = v; break;
        case 0x8C4: /* Falcon firmware call 4 signals completion through shadow scratch 0. */ R3(0xD00) = 1; break;
        case 0x6C: i2m_launch(ch, ch->r3d, 0x60); break;
        case 0x6D: i2m_data(ch, v); break;
        case 0xB2: /* SyncInfo */
            if (v & (1u << 20))
                g_renderer->stream_incr(v & 0xFFFF);
            else { /* Ryujinx increments on every write of SyncpointAction: does the game write it with bit 20 clear? */
                LOG_LIMIT(4, "gpu: 3D syncpoint action %x without bit 20, not incremented\n", v);
            }
            break;
        case 0x674: r3d_clear(ch, v); break;                     /* ClearBuffers */
        case 0x556:                                              /* SetRenderEnableC: the mode */
        case 0x651: ch->render_off = !render_enabled(ch); break; /* the override */
        case 0x42B: r3d_draw_texture(ch); break;                 /* DRAW_TEXTURE: its last word starts it */
        case 0x4C1:                                              /* four u8 inline indices */
            for (u32 i = 0; i < 4; i++) inline_index_push(ch, (v >> (i * 8)) & 0xFF);
            break;
        case 0x57A: inline_index_push(ch, v); break; /* one u32 inline index */
        case 0x57C:                                  /* two u16 inline indices */
            inline_index_push(ch, v & 0xFFFF);
            inline_index_push(ch, v >> 16);
            break;
        case 0x6C3: query_get(ch, ch->r3d, 0x6C0, v); break;
        case 0x585: { /* END: flush */
            if (g_pd.on && g_pd.indexed == 3) r3d_pending_flush();
            u32 topo = R3(0x586) & 0xFFFF;
            int idx = ch->inline_n ? 3 : R3(0x5F8) != 0;
            u32 first = idx == 3 ? 0 : idx ? R3(0x5F7) : R3(0x35D);
            u32 count = idx == 3 ? ch->inline_n : idx ? R3(0x5F8) : R3(0x35E);
            u32 bv = idx ? R3(0x50D) : 0, bi = R3(0x50E);
            if (idx != 3 && g_pd.on && g_pd.ch == ch && g_pd.small_kind == ~0u && ((R3(0x586) >> 26) & 1) &&
                g_pd.indexed == idx && g_pd.topo == topo && g_pd.first == first && g_pd.count == count &&
                g_pd.bv == bv && g_pd.bi == bi) {
                g_pd.inst = ch->instance_count - g_pd.first_instance + 1;
            } /* one more instance */
            else {
                r3d_pending_flush();
                g_pd.on = 1;
                g_pd.ch = ch;
                g_pd.indexed = idx;
                g_pd.topo = topo;
                g_pd.first = first;
                g_pd.count = count;
                g_pd.bv = bv;
                g_pd.bi = bi;
                g_pd.first_instance = ch->instance_count;
                g_pd.inst = 1;
                g_pd.small_kind = ~0u;
            }
            /* hw consumes the counts on each draw; if we don't, the next non-indexed draw picks up stale indices */
            R3(0x5F8) = 0;
            R3(0x35E) = 0;
            break;
        }
        case 0x485:   /* VERTEX_ARRAY_INSTANCE_FIRST: vertices first .. first + count - 1 now, as instance 0 */
        case 0x486: { /* _SUBSEQUENT: the same range again, as the next instance. Word: first 0-15, count 16-27,
                         topology 28-31 */
            u32 first = v & 0xFFFF, count = (v >> 16) & 0xFFF, topo = (v >> 28) & 0xF, bi = R3(0x50E);
            if (m == 0x485)
                ch->instance_count = 0;
            else
                ch->instance_count++;
            if (m == 0x486 && g_pd.on && g_pd.ch == ch && !g_pd.indexed && g_pd.topo == topo && g_pd.first == first &&
                g_pd.count == count && g_pd.bv == 0 && g_pd.bi == bi) {
                g_pd.inst = ch->instance_count - g_pd.first_instance + 1; /* one more instance */
                break;
            }
            r3d_pending_flush();
            g_pd.on = 1;
            g_pd.ch = ch;
            g_pd.indexed = 0;
            g_pd.topo = topo;
            g_pd.first = first;
            g_pd.count = count;
            g_pd.bv = 0;
            g_pd.bi = bi;
            g_pd.first_instance = ch->instance_count;
            g_pd.inst = 1;
            g_pd.small_kind = ~0u;
            break;
        }
        case 0x586: /* BEGIN, bit 26 = next instance */
            if (v & (1u << 26))
                ch->instance_count++;
            else if (!(v & (1u << 27)))
                ch->instance_count = 0;
            break;
        case 0x5F9: /* INDEX_BUFFER{32,16,8}_{FIRST,SUBSEQUENT}: packed first/count/topology */
        case 0x5FA:
        case 0x5FB:
        case 0x5FC:
        case 0x5FD:
        case 0x5FE: {
            u32 first = v & 0xFFFF, count = (v >> 16) & 0xFFF, topo = (v >> 28) & 0xF;
            u32 bv = R3(0x50D), bi = R3(0x50E);
            if (m < 0x5FC)
                ch->instance_count = 0;
            else
                ch->instance_count++;
            if (m >= 0x5FC && g_pd.on && g_pd.ch == ch && g_pd.indexed == 1 && g_pd.small_kind == (m - 0x5F9) % 3 &&
                g_pd.topo == topo && g_pd.first == first && g_pd.count == count && g_pd.bv == bv && g_pd.bi == bi) {
                g_pd.inst = ch->instance_count - g_pd.first_instance + 1;
                break;
            }
            r3d_pending_flush();
            g_pd.on = 1;
            g_pd.ch = ch;
            g_pd.indexed = 1;
            g_pd.topo = topo;
            g_pd.first = first;
            g_pd.count = count;
            g_pd.bv = bv;
            g_pd.bi = bi;
            g_pd.first_instance = ch->instance_count;
            g_pd.inst = 1;
            g_pd.small_kind = (m - 0x5F9) % 3;
            break;
        }
        case 0x8E4:
        case 0x8E5:
        case 0x8E6:
        case 0x8E7:
        case 0x8E8:
        case 0x8E9:
        case 0x8EA:
        case 0x8EB:
        case 0x8EC:
        case 0x8ED:
        case 0x8EE:
        case 0x8EF:
        case 0x8F0:
        case 0x8F1:
        case 0x8F2:
        case 0x8F3: r3d_const_upload(ch, v, last); break;
        case 0x904:
        case 0x90C:
        case 0x914:
        case 0x91C:
        case 0x924: r3d_cb_bind(ch, (int)((m - 0x904) / 8), v); break;
        default: break;
    }
}
