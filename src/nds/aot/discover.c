/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Offline discovery of the image's code, after the 3DS's (3ds/aot/discover.c): from the entry point, the overlays'
   static initializers and the blocks earlier runs reached, along every direct branch and call. Each instruction slot
   is visited once in each instruction set. A target that lies in several regions (overlays that share an address) is
   an entry of each.

   A function reached only through its address (a virtual function, a callback) is found from the words that hold the
   address. The image's code and data are not apart, and data is full of words that look like addresses, so such a
   word is taken only where a function can start: at code that begins as a function does, right after the end of code
   already found (a return, a branch, the words a function loads its constants from) or at a region's start. The
   functions found that way end others' neighbors, so the words are gone over again until none is taken. What this
   leaves out compiles when a run reaches it, and is a seed the next time. */
#include "nds/aot/nds_module.h"
#include "common/cpu/arm/arm.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

#define QUEUED  8u
#define LITERAL 16u /* a word code loads as data: on the ARM flags, by word */
#define END     32u /* an instruction the code does not go on after */
#define PROFILE SNX_ARM_PROFILE_ARM946E

typedef struct {
    uint32_t region, pc; /* pc | 1 for Thumb */
} Work;
typedef struct {
    const NdsRom* rom;
    NdsPlan* plans;
    Work* queue;
    size_t count, capacity;
    Work* maybe; /* the places words name that code can start at, taken when what is before them is found */
    size_t maybe_count, maybe_capacity;
    bool failed;
} Discovery;

static bool inside(const NdsRegion* r, uint32_t pc, bool thumb) {
    unsigned width = thumb ? 2 : 4;
    return !(pc & (width - 1)) && pc >= r->address && (uint64_t)pc + width <= (uint64_t)r->address + r->size;
}
static unsigned char* flag(const NdsRegion* r, NdsPlan* p, uint32_t pc, bool thumb) {
    return (thumb ? p->thumb : p->arm) + (pc - r->address) / (thumb ? 2 : 4);
}
static void enqueue_in(Discovery* d, unsigned region, uint32_t pc, bool thumb) {
    const NdsRegion* r = &d->rom->regions[region];
    if (!inside(r, pc, thumb)) return;
    unsigned char* f = flag(r, &d->plans[region], pc, thumb);
    *f |= NDS_PLAN_ENTRY;
    if (*f & (QUEUED | NDS_PLAN_VISITED)) return;
    if (d->count == d->capacity) {
        size_t capacity = d->capacity ? d->capacity * 2 : 4096;
        Work* grown = realloc(d->queue, capacity * sizeof *grown);
        if (!grown) {
            d->failed = true;
            return;
        }
        d->queue = grown, d->capacity = capacity;
    }
    *f |= QUEUED;
    d->queue[d->count++] = (Work){region, pc | (uint32_t)thumb};
}
/* every region that holds the address: the one the branch is in first, as code calls its own region most */
static void enqueue(Discovery* d, unsigned from, uint32_t pc, bool thumb) {
    enqueue_in(d, from, pc, thumb);
    for (unsigned i = 0; i < d->rom->region_count; ++i)
        if (i != from) enqueue_in(d, i, pc, thumb);
}
static bool read_word(const NdsRegion* r, uint32_t address, uint32_t* value) {
    if ((address & 3) || address < r->address || (uint64_t)address + 4 > (uint64_t)r->address + r->size) return false;
    *value = snx_ld_le32(r->data + (address - r->address));
    return true;
}
/* whether the word at address in the region is an ARM instruction that always runs */
static bool arm_always(const NdsRegion* r, uint32_t address) {
    uint32_t w;
    return read_word(r, address, &w) && (w >> 28) == 14 && snx_a32_decode_profile(PROFILE, w) != SNX_A32_UNDEFINED;
}
static void literal(Discovery* d, unsigned region, uint32_t address) {
    const NdsRegion* r = &d->rom->regions[region];
    if (inside(r, address & ~3u, false)) *flag(r, &d->plans[region], address & ~3u, false) |= LITERAL;
}
/* Whether what the region holds at pc reads as code: its first instructions, up to eight or the first that ends the
   code. ARM: every one an instruction, and three in four of them ones that always run (a small number, an address
   and text have another condition in their top bits, and a word of zeros is one of those). Thumb: every one an
   instruction and none a halfword of zeros, in a place that is not ARM code, whose words read as halfwords look like
   Thumb's branches */
static bool reads_as_code(const NdsRegion* r, uint32_t pc, bool thumb) {
    if (thumb) {
        uint32_t word = pc & ~3u;
        unsigned branches = 0;
        if (arm_always(r, word) + arm_always(r, word + 4) + arm_always(r, word + 8) + arm_always(r, word + 12) >= 3)
            return false;
        for (unsigned n = 0; n < 8; ++n, pc += 2) {
            SnxThumb t;
            if (!inside(r, pc, true)) return n >= 2;
            uint16_t h = snx_ld_le16(r->data + (pc - r->address));
            if (!h || !snx_thumb_decode_profile(PROFILE, h, 0, &t)) return false;
            if ((h & 0xF000) == 0xE000 && ++branches > 1) return false;
            if ((h & 0xFF00) == 0xBD00 || (h & 0xFF00) == 0x4700) break; /* POP {pc}, BX */
        }
        return true;
    }
    unsigned always = 0, n = 0;
    for (; n < 8; ++n, pc += 4) {
        uint32_t w;
        if (!read_word(r, pc, &w)) return n >= 2 && always * 4 >= n * 3;
        SnxA32Kind k = snx_a32_decode_profile(PROFILE, w);
        if (!w || (w >> 28) == 15) return false;
        if (k == SNX_A32_UNDEFINED && (w & 0x0FBF0FFF) != 0x010F0000 && (w & 0x0FB0FFF0) != 0x0120F000 &&
            (w & 0x0FB0F000) != 0x0320F000)
            return false;
        always += (w >> 28) == 14;
        /* a return or a jump that always runs: BX, B, LDM with the PC, a write to the PC */
        if ((w >> 28) == 14 && (k == SNX_A32_BX || (k == SNX_A32_BRANCH && !(w & (1u << 24))) ||
                                (k == SNX_A32_MULTIPLE && (w & 0x00108000) == 0x00108000) ||
                                (k == SNX_A32_DATA && ((w >> 12) & 15) == 15 && (((w >> 21) & 15) < 8 || ((w >> 21) & 15) > 11)))) {
            ++n;
            break;
        }
    }
    return always * 4 >= n * 3;
}
/* A word that may be a function's address: kept, for each region that holds the address, when what is there reads as
   code. It is taken once the code found so far ends just before it (follows_code) */
static void pointer(Discovery* d, uint32_t value) {
    bool thumb = value & 1;
    uint32_t pc = value & ~1u;
    for (unsigned i = 0; i < d->rom->region_count; ++i) {
        const NdsRegion* r = &d->rom->regions[i];
        if (!inside(r, pc, thumb) || !reads_as_code(r, pc, thumb)) continue;
        /* data reads as Thumb code nearly everywhere, and Thumb code decoded from data finds more of it: a Thumb
           function a word names is taken when it starts with PUSH, as all but leaf functions do */
        if (thumb && (snx_ld_le16(r->data + (pc - r->address)) & 0xFE00) != 0xB400) continue;
        if (d->maybe_count == d->maybe_capacity) {
            size_t capacity = d->maybe_capacity ? d->maybe_capacity * 2 : 4096;
            Work* grown = realloc(d->maybe, capacity * sizeof *grown);
            if (!grown) {
                d->failed = true;
                return;
            }
            d->maybe = grown, d->maybe_capacity = capacity;
        }
        d->maybe[d->maybe_count++] = (Work){i, pc | (uint32_t)thumb};
    }
}
/* a call that changes the instruction set, from code found: taken where what it calls reads as code */
static void enqueue_exchange(Discovery* d, unsigned from, uint32_t pc, bool thumb) {
    for (unsigned n = 0; n < d->rom->region_count; ++n) {
        unsigned i = n ? (n <= from ? n - 1 : n) : from;
        const NdsRegion* r = &d->rom->regions[i];
        if (inside(r, pc, thumb) && reads_as_code(r, pc, thumb)) enqueue_in(d, i, pc, thumb);
    }
}
/* whether code found so far ends just before pc, so a function can start there */
static bool follows_code(const Discovery* d, unsigned region, uint32_t pc, bool thumb) {
    const NdsRegion* r = &d->rom->regions[region];
    NdsPlan* p = &d->plans[region];
    if (pc == r->address) return true;
    for (unsigned step = 0; step < 2; ++step) {
        /* the word before: a literal, or an ARM instruction that ends its code */
        if (pc >= r->address + 4 && !(pc & 3)) {
            unsigned f = *flag(r, p, pc - 4, false);
            if ((f & LITERAL) || ((f & NDS_PLAN_VISITED) && (f & END))) return true;
        }
        if (pc >= r->address + 2) {
            unsigned f = *flag(r, p, pc - 2, true);
            if ((f & NDS_PLAN_VISITED) && (f & END)) return true;
            if (*flag(r, p, (pc - 2) & ~3u, false) & LITERAL) return true;
            /* a Thumb function after two bytes that fill up to a word */
            uint16_t h = snx_ld_le16(r->data + (pc - 2 - r->address));
            if (!thumb || step || (h != 0x0000 && h != 0x46C0 && h != 0x1C00)) return false;
            pc -= 2;
        } else
            return false;
    }
    return false;
}
static uint32_t immediate(uint32_t w) {
    uint32_t v = w & 255;
    unsigned r = ((w >> 8) & 15) * 2;
    return r ? (v >> r) | (v << (32 - r)) : v;
}
/* whether the instruction ends the block */
static bool arm(Discovery* d, unsigned region, uint32_t pc, uint32_t w) {
    const NdsRegion* r = &d->rom->regions[region];
    SnxA32Kind k = snx_a32_decode_profile(PROFILE, w);
    unsigned rd = (w >> 12) & 15, condition = w >> 28;
    bool conditional = condition < 14, stop = false, fall = conditional;
    if (k == SNX_A32_BRANCH || k == SNX_A32_BLX_IMM) {
        uint32_t off = (w & 0xffffff) << 2;
        if (off & 0x02000000) off |= 0xfc000000;
        if (k == SNX_A32_BLX_IMM) off |= (w >> 23) & 2;
        if (k == SNX_A32_BLX_IMM)
            enqueue_exchange(d, region, pc + 8 + off, true);
        else
            enqueue(d, region, pc + 8 + off, false);
        stop = true;
        fall |= !!(w & (1u << 24)) || k == SNX_A32_BLX_IMM;
    } else if (k == SNX_A32_BX) {
        stop = true;
        fall |= !!(w & 0x20);
    } else if (k == SNX_A32_DATA) {
        unsigned op = (w >> 21) & 15, rn = (w >> 16) & 15;
        if ((w & (1u << 25)) && rn == 15 && (op == 2 || op == 4)) {
            uint32_t target = op == 4 ? pc + 8 + immediate(w) : pc + 8 - immediate(w);
            if (rd == 15)
                enqueue(d, region, target & ~3u, false);
            else
                pointer(d, target);
        }
        stop = rd == 15 && !(op >= 8 && op <= 11);
        /* ADD PC, PC, Rm LSL #2: a table of branches follows, as a switch compiles to. The compare before it says
           how many; without one, a few */
        if (stop && op == 4 && rn == 15 && !(w & (1u << 25))) {
            uint32_t bound, cases = 16;
            if (read_word(r, pc - 4, &bound) && (bound & 0xfff0f000u) == 0xe3500000u && immediate(bound) < 1024)
                cases = immediate(bound) + 1;
            for (unsigned i = 0; i < cases; ++i) enqueue_in(d, region, pc + 8 + i * 4, false);
        }
    } else if (k == SNX_A32_SINGLE || k == SNX_A32_EXTRA) {
        stop = rd == 15 && !!(w & (1u << 20));
        /* CMP Rm, #N; LDRLS PC, [PC, Rm, LSL #2]: a table of N + 1 addresses */
        uint32_t bound;
        if (k == SNX_A32_SINGLE && (w & 0xfffffff0u) == 0x979ff100u && read_word(r, pc - 4, &bound) &&
            (bound & 0xfff0f000u) == 0xe3500000u && ((bound >> 16) & 15) == (w & 15) && immediate(bound) < 1024) {
            for (uint32_t i = 0; i <= immediate(bound); ++i) {
                uint32_t target;
                literal(d, region, pc + 8 + i * 4);
                if (read_word(r, pc + 8 + i * 4, &target)) enqueue(d, region, target & ~1u, target & 1);
            }
        }
        /* a word loaded from the literal pool */
        if (k == SNX_A32_SINGLE && ((w >> 16) & 15) == 15 && !(w & ((1u << 25) | (1u << 22))) && (w & (1u << 24)) &&
            (w & (1u << 20))) {
            uint32_t value, at = (w & (1u << 23)) ? pc + 8 + (w & 4095) : pc + 8 - (w & 4095);
            literal(d, region, at);
            if (read_word(r, at, &value)) {
                if (rd == 15)
                    enqueue(d, region, value & ~1u, value & 1);
                else
                    pointer(d, value);
            }
        }
    } else if (k == SNX_A32_MULTIPLE)
        stop = (w & (1u << 15)) && (w & (1u << 20));
    else if (k == SNX_A32_SVC) {
        stop = true;
        fall = true;
    } else if (k == SNX_A32_BKPT || k == SNX_A32_UNDEFINED) {
        /* the system's instructions run and go on (cpu/cpu.c); a word that is no instruction ends the code */
        /* the system's instructions run in the runtime and the code goes on after them (cpu/cpu.c): MRS, MSR, SWP and
           the LDM and STM of the user bank's registers. Any other word the profile does not have ends the code: data
           decoded as code is full of them, and going on past each would take all of it for code */
        stop = true;
        fall = k == SNX_A32_UNDEFINED && condition != 15 &&
               ((w & 0x0FBF0FFF) == 0x010F0000 || (w & 0x0FB0FFF0) == 0x0120F000 || (w & 0x0FB0F000) == 0x0320F000 ||
                (w & 0x0FB00FF0) == 0x01000090 || ((w & 0x0E400000) == 0x08400000 && (w & 0x00108000) != 0x00108000));
    } else if (k == SNX_A32_COPROC)
        stop = false;
    if (stop && fall) enqueue_in(d, region, pc + 4, false);
    if (stop && !fall) *flag(r, &d->plans[region], pc, false) |= END;
    return stop;
}
static bool thumb(Discovery* d, unsigned region, uint32_t pc, uint16_t h) {
    const NdsRegion* r = &d->rom->regions[region];
    SnxThumb t;
    if (!snx_thumb_decode_profile(PROFILE, h, 0, &t)) return true;
    bool stop = false, fall = false;
    if (t.shape == TH_LEGACY_BL) {
        if (!t.op) {
            if (inside(r, pc + 2, true)) {
                SnxThumb suffix;
                uint16_t next = snx_ld_le16(r->data + (pc + 2 - r->address));
                if (snx_thumb_decode_profile(PROFILE, next, 0, &suffix) && suffix.shape == TH_LEGACY_BL && suffix.op) {
                    uint32_t off = t.imm << 12;
                    if (t.imm & 0x400) off |= 0xff800000;
                    uint32_t target = pc + 4 + off + (suffix.imm << 1);
                    if (suffix.op == 2)
                        enqueue_exchange(d, region, target & ~3u, false);
                    else
                        enqueue(d, region, target & ~1u, true);
                }
            }
        } else {
            stop = true;
            fall = true;
        }
    } else if (t.shape == TH_BRANCH) {
        stop = true;
        if (t.op == BR_B || t.op == BR_BCOND) {
            uint32_t off = t.imm << 1;
            if (t.op == BR_B && (off & 0x800)) off |= 0xfffff000;
            if (t.op == BR_BCOND && (off & 0x100)) off |= 0xfffffe00;
            enqueue(d, region, pc + 4 + off, true);
            fall = t.op == BR_BCOND;
        } else if (t.op == BR_BLX_REG || t.op == BR_SVC)
            fall = true;
    } else if (t.shape == TH_HI)
        stop = (t.op != 1 && t.rd == 15) || t.op == 3;
    else if (t.shape == TH_PUSHPOP)
        stop = t.op == 1 && t.ra;
    else if (t.shape == TH_LOAD && (t.flags & TH_PCB)) {
        uint32_t value;
        literal(d, region, ((pc + 4) & ~3u) + t.imm);
        if (read_word(r, ((pc + 4) & ~3u) + t.imm, &value)) pointer(d, value);
    }
    if (stop && fall) enqueue_in(d, region, pc + 2, true);
    if (stop && !fall) *flag(r, &d->plans[region], pc, true) |= END;
    return stop;
}

void nds_plans_free(NdsPlan* plans, unsigned count) {
    if (!plans) return;
    for (unsigned i = 0; i < count; ++i) {
        free(plans[i].arm);
        free(plans[i].thumb);
    }
    free(plans);
}

unsigned nds_plan_length(const NdsRegion* r, const NdsPlan* p, uint32_t pc, bool thumb_mode) {
    if (!inside(r, pc, thumb_mode)) return 0;
    const unsigned char* flags = thumb_mode ? p->thumb : p->arm;
    unsigned i = (pc - r->address) / (thumb_mode ? 2 : 4), limit = r->size / (thumb_mode ? 2 : 4), n = 0;
    if (!(flags[i] & NDS_PLAN_ENTRY)) return 0;
    while (i < limit && n < NDS_BLOCK_UNITS && (flags[i] & NDS_PLAN_VISITED)) {
        unsigned f = flags[i++];
        ++n;
        if ((f & NDS_PLAN_STOP) || (i < limit && (flags[i] & NDS_PLAN_ENTRY))) break;
    }
    return n;
}

bool nds_discover(const NdsRom* rom, const uint32_t* seeds, size_t seed_count, NdsPlan** out, uint64_t* entries) {
    NdsPlan* plans = calloc(rom->region_count ? rom->region_count : 1, sizeof *plans);
    Discovery d = {.rom = rom, .plans = plans};
    *out = NULL;
    if (!plans) return false;
    for (unsigned i = 0; i < rom->region_count; ++i) {
        plans[i].arm = calloc(rom->regions[i].size / 4 + 1, 1);
        plans[i].thumb = calloc(rom->regions[i].size / 2 + 1, 1);
        if (!plans[i].arm || !plans[i].thumb) goto fail;
    }
    for (unsigned i = 0; i < rom->region_count; ++i) {
        const NdsRegion* r = &rom->regions[i];
        for (uint32_t e = 0; e < r->entry_count; ++e) enqueue(&d, i, r->entries[e] & ~1u, r->entries[e] & 1);
        /* the words of its own that look like functions' addresses: tables of handlers, of virtual functions */
        for (uint32_t at = 0; at + 4 <= r->size; at += 4) {
            uint32_t value = snx_ld_le32(r->data + at);
            if (value >= 0x01000000u && value < 0x04000000u) pointer(&d, value);
        }
    }
    for (size_t i = 0; i + 1 < seed_count * 2; i += 2)
        if (seeds[i] < rom->region_count) enqueue_in(&d, seeds[i], seeds[i + 1] & ~1u, seeds[i + 1] & 1);
    size_t head = 0;
    for (bool more = true; more && !d.failed;) {
        for (; head < d.count && !d.failed; ++head) {
            unsigned region = d.queue[head].region;
            const NdsRegion* r = &rom->regions[region];
            bool t = d.queue[head].pc & 1;
            uint32_t pc = d.queue[head].pc & ~1u;
            for (unsigned n = 0; n < NDS_BLOCK_UNITS && inside(r, pc, t); ++n) {
                unsigned char* f = flag(r, &plans[region], pc, t);
                if (*f & NDS_PLAN_VISITED) break;
                *f |= NDS_PLAN_VISITED;
                const unsigned char* at = r->data + (pc - r->address);
                bool stop = t ? thumb(&d, region, pc, snx_ld_le16(at)) : arm(&d, region, pc, snx_ld_le32(at));
                if (stop) {
                    *f |= NDS_PLAN_STOP;
                    break;
                }
                pc += t ? 2 : 4;
                if (n + 1 == NDS_BLOCK_UNITS) enqueue_in(&d, region, pc, t);
            }
        }
        /* the addresses words name, where the code found so far lets a function start */
        more = false;
        for (size_t i = 0; i < d.maybe_count;) {
            Work m = d.maybe[i];
            const NdsRegion* r = &rom->regions[m.region];
            unsigned f = *flag(r, &plans[m.region], m.pc & ~1u, m.pc & 1);
            bool known = (f & (NDS_PLAN_VISITED | QUEUED)) != 0;
            if (!known && !follows_code(&d, m.region, m.pc & ~1u, m.pc & 1)) {
                ++i;
                continue;
            }
            enqueue_in(&d, m.region, m.pc & ~1u, m.pc & 1);
            d.maybe[i] = d.maybe[--d.maybe_count];
            more = more || !known;
        }
    }
    if (d.failed) goto fail;
    *entries = 0;
    for (unsigned i = 0; i < rom->region_count; ++i) {
        const NdsRegion* r = &rom->regions[i];
        for (uint32_t s = 0; s < r->size / 4; ++s) *entries += (plans[i].arm[s] & NDS_PLAN_ENTRY) != 0;
        for (uint32_t s = 0; s < r->size / 2; ++s) *entries += (plans[i].thumb[s] & NDS_PLAN_ENTRY) != 0;
    }
    free(d.queue);
    free(d.maybe);
    *out = plans;
    return true;
fail:
    free(d.queue);
    free(d.maybe);
    nds_plans_free(plans, rom->region_count);
    return false;
}
