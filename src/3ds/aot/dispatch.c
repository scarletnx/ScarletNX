/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/aot/ctr_discover.h"
#include "3ds/aot/ctr_hook.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/kernel/ctr_kernel.h"
#include "3ds/runtime/internal.h"
#include "3ds/memory/host.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned snx_ctr_module_memory(void) { return ctr_host_flat() ? SNX_CTR_MODULE_HOSTED_MODE : SNX_CTR_MODULE_CHECKED_MODE; }
bool snx_ctr_module_compatible(const SnxCtrModule* m, const SnxCtrImage* image) {
    return snx_ctr_module_fields(m, image) && m->arm_blocks && m->thumb_blocks;
}
bool snx_ctr_module_fields(const SnxCtrModule* m, const SnxCtrImage* image) {
    return m && image && m->magic == SNX_CTR_MODULE_MAGIC && m->abi == SNX_CTR_MODULE_ABI &&
           m->descriptor_size == sizeof *m && m->cpu_size == sizeof(SnxArmCPU) &&
           m->checked_size == sizeof(SnxArmCheckedCPU) && m->arm_abi == SNX_ARM_ABI &&
           m->checked_abi == SNX_ARM_CHECKED_ABI && m->codegen_abi == SNX_CTR_MODULE_CODEGEN_ABI &&
           m->profile == SNX_ARM_PROFILE_ARM11 && m->memory_mode == snx_ctr_module_memory() &&
           m->pointer_size == sizeof(void*) && m->text_address == image->segment[0].address &&
           m->text_bytes == image->segment[0].bytes && m->text_bytes && !(m->text_bytes & 3) &&
           m->text_bytes <= SNX_CTR_AOT_TEXT_LIMIT && m->arm_count == m->text_bytes / 4 &&
           m->thumb_count == m->text_bytes / 2 &&
           !memcmp(m->image_identity, image->identity, 32);
}
/* Schedule a thread to run: false when none runs now, with the dispatch's result in stop. An idle wake (a vblank or
   an audio frame that woke nobody) ends the dispatch as a spent budget, so the caller can look at its window and
   dispatch again; no thread ever running again is BLOCKED. An SVC or a hook inside a core's slice (stay) serves that
   core first; the slice, blocks counted in *slice, starts again when the turn passes to another core or it ended. */
static bool scheduled(SnxCtrProcess* process, SnxCtrRunResult* stop, bool stay, uint32_t* slice) {
    uint32_t core = process->next_core;
    process->stay_on_core = stay;
    SnxCtrSchedule s = snx_ctr_schedule(process);
    if (!stay || process->next_core != core) *slice = 0;
    *stop = s == SNX_CTR_SCHED_IDLE ? SNX_CTR_RUN_BUDGET : SNX_CTR_RUN_BLOCKED;
    return s == SNX_CTR_SCHED_RUN;
}
/* A block of the static tables takes at most SNX_CTR_AOT_BLOCK_WORDS ARM instructions (fewer bytes in Thumb). One
   that starts in the window of a rewritten word or in the window before it can hold that word. */
#define WINDOW_SHIFT 7
#define WINDOW_BYTES (1u << WINDOW_SHIFT)
typedef char window_is_the_arm_block_span[SNX_CTR_AOT_BLOCK_WORDS * 4u == WINDOW_BYTES ? 1 : -1];
static bool window_marked(const SnxCtrProcess* process, uint32_t offset) {
    uint32_t window = offset >> WINDOW_SHIFT;
    return process->aot_stale && ((process->aot_stale[window / 32] >> (window % 32)) & 1);
}
bool snx_ctr_aot_mark_stale(SnxCtrProcess* process, uint32_t address, uint32_t size) {
    uint64_t text = process->segment[0].address, end = text + process->segment[0].bytes;
    uint64_t from = address > text ? address : text, to = (uint64_t)address + size;
    if (to > end) to = end;
    if (from >= to) return true;
    if (!process->aot_stale) {
        size_t windows = ((size_t)process->segment[0].bytes + WINDOW_BYTES - 1) >> WINDOW_SHIFT;
        process->aot_stale = calloc((windows + 31) / 32, sizeof *process->aot_stale);
        if (!process->aot_stale) return false;
    }
    uint64_t first = from - text, last = to - 1 - text;
    uint64_t earliest = first < WINDOW_BYTES ? 0 : first - (WINDOW_BYTES - 1);
    for (uint64_t window = earliest >> WINDOW_SHIFT; window <= last >> WINDOW_SHIFT; ++window)
        process->aot_stale[window / 32] |= 1u << (window % 32);
    return true;
}
/* The block an address ran, kept from the static tables: a direct-mapped cache, because the same few thousand
   addresses come back every frame and each costs the hook table's bit test, the text's bounds and the tables' load.
   Only an entry the tables hold is kept: one the run-time compiler made can be remade when its text changes. */
#define BLOCK_CACHE 16384u
typedef struct {
    uint32_t key;     /* the address with the Thumb bit, 0 for an empty entry */
    uint32_t unused;
    uintptr_t target; /* the block, or the hook with its low bit set: both are aligned */
} BlockCache;
static BlockCache g_blocks[BLOCK_CACHE];
static uint64_t g_blocks_epoch; /* the space's mappings the entries were checked under */
static const SnxCtrModule* g_blocks_module;
static const SnxCtrProcess* g_blocks_process;
/* a process or module that goes takes its blocks with it: the next dispatch starts the cache empty */
void snx_ctr_dispatch_forget(void) {
    g_blocks_module = NULL;
    g_blocks_process = NULL;
}
SnxCtrRunResult snx_ctr_dispatch(const SnxCtrModule* m, SnxCtrProcess* process, uint64_t limit, uint64_t* executed) {
    if (!executed) return SNX_CTR_RUN_FAULT;
    *executed = 0;
    SnxArmCPU* cpu = snx_ctr_process_cpu(process);
    if (!cpu || !m) return SNX_CTR_RUN_FAULT;
    SnxCtrRunResult stop;
    uint32_t since_poll = 0;
    if (!scheduled(process, &stop, false, &since_poll)) return stop;
    SnxCtrAddressSpace* space = snx_ctr_process_space(process);
    uint64_t map_epoch = snx_ctr_space_map_epoch(space);
    if (g_blocks_module != m || g_blocks_process != process) { /* a new module's or process's blocks */
        memset(g_blocks, 0, sizeof g_blocks);
        g_blocks_module = m;
        g_blocks_process = process;
        g_blocks_epoch = map_epoch;
    }
    /* the entries' execute checks hold while the space's mappings do: a change makes them all stale */
    if (g_blocks_epoch != map_epoch) {
        memset(g_blocks, 0, sizeof g_blocks);
        g_blocks_epoch = map_epoch;
    }
    bool cacheable = !process->aot_stale;
    uint64_t count = 0;
    while (count < limit) {
        if (process->reclaim) process->reclaim(process->compiler);
        /* Every block returns here, so a loop that makes no SVC still reaches the scheduler: due vblanks, audio frames,
           timers and timeouts are delivered, and a thread they wake with a better priority takes the core, as the
           interrupt would preempt it on the console */
        if (++since_poll == SNX_CTR_POLL_BLOCKS && !scheduled(process, &stop, false, &since_poll)) {
            *executed += count;
            return stop;
        }
        uint32_t pc = cpu->r[15];
        bool thumb = !!(cpu->cpsr & SNX_ARM_T);
        uint32_t key = pc | (uint32_t)thumb;
        BlockCache* entry = &g_blocks[((pc >> 2) ^ (thumb ? BLOCK_CACHE / 2 : 0)) & (BLOCK_CACHE - 1)];
        /* a block of the tables at this address, run again within the space's same mappings: its page's execute
           permission was checked when the entry was made, and only a mapping change can take it away */
        bool cached = cacheable && key && entry->key == key;
        /* a function the runtime runs itself (aot/ctr_hook.h), unless a CRO's relocation rewrote its text. It
           reschedules as an SVC does when it woke a thread, which can be one that takes the core now */
        const struct SnxCtrHook* hook = NULL;
        SnxArmBlock block = NULL;
        bool compiled = false;
        if (cached) {
            uintptr_t target = entry->target;
            if (target & 1)
                hook = (const struct SnxCtrHook*)(target & ~(uintptr_t)1);
            else
                block = (SnxArmBlock)target;
        } else {
            unsigned width = thumb ? 2 : 4;
            if (pc & (width - 1)) {
                cpu->exit = SNX_ARM_ALIGNMENT;
                cpu->syndrome = pc;
                *executed += count;
                return SNX_CTR_RUN_FAULT;
            }
            /* an address the tables do not hold, or one whose page may have changed: the console faults on a prefetch
               into a page it cannot run, and a game can take that permission away */
            if (snx_ctr_check_exec(space, pc, width) != SNX_CTR_MEM_OK) {
                cpu->exit = SNX_ARM_PREFETCH_ABORT;
                cpu->syndrome = pc;
                *executed += count;
                return SNX_CTR_RUN_FAULT;
            }
            bool from_table = false;
            hook = process->hooks && !thumb ? snx_ctr_hook_at(process->hooks, pc) : NULL;
            if (hook && window_marked(process, pc - m->text_address)) hook = NULL;
            if (pc >= m->text_address && (uint64_t)pc + width <= (uint64_t)m->text_address + m->text_bytes &&
                !window_marked(process, pc - m->text_address)) {
                uint32_t index = (pc - m->text_address) / width;
                block = thumb ? m->thumb_blocks[index] : m->arm_blocks[index];
                from_table = block != NULL;
            }
            /* code the static discovery didn't reach (an indirect target, a module loaded at run time) compiles now */
            if (!block && process->compile)
                compiled = (block = process->compile(process->compiler, pc, thumb, false)) != NULL;
            if (!block && !hook) {
                cpu->exit = SNX_ARM_BACKEND_UNSUPPORTED;
                cpu->syndrome = pc;
                *executed += count;
                return SNX_CTR_RUN_UNSUPPORTED;
            }
            if (!process->aot_stale && (from_table || (hook && !block)))
                *entry = (BlockCache){key, 0,
                                      hook ? (uintptr_t)hook | (uintptr_t)1 : (uintptr_t)block};
        }
        if (hook) {
            uint64_t sequence = process->sched_seq;
            SnxCtrHookResult ran = snx_ctr_hook_run(process, cpu, hook);
            if (ran == SNX_CTR_HOOK_DONE) {
                ++count;
                uint64_t epoch = snx_ctr_space_map_epoch(space);
                if (epoch != map_epoch) {
                    memset(g_blocks, 0, sizeof g_blocks);
                    map_epoch = g_blocks_epoch = epoch;
                }
                cacheable = !process->aot_stale;
                if (process->sched_seq != sequence && !scheduled(process, &stop, true, &since_poll)) {
                    *executed += count;
                    return stop;
                }
                continue;
            }
            if (ran == SNX_CTR_HOOK_STOP) {
                *executed += count;
                return cpu->exit == SNX_ARM_BACKEND_UNSUPPORTED ? SNX_CTR_RUN_UNSUPPORTED : SNX_CTR_RUN_FAULT;
            }
            if (!block) { /* the hook declined and the discovery found no block there */
                cpu->exit = SNX_ARM_BACKEND_UNSUPPORTED;
                cpu->syndrome = pc;
                *executed += count;
                return SNX_CTR_RUN_UNSUPPORTED;
            }
        }
        /* a hosted block (SNX_ARM_MEMORY_HOSTED, llvm.h) takes its entry state from this loop: r15 and CPSR.T chose the
           block, exit and syndrome are clear (the context of a thread that blocked in an SVC comes back with them
           set), and no IT block is under way, which an SVC inside one or a stop at a changed instruction leaves */
        cpu->exit = SNX_ARM_OK;
        cpu->syndrome = 0;
        if (thumb && snx_arm_itstate(cpu->cpsr)) {
            cpu->exit = SNX_ARM_IT_ENTRY;
            cpu->syndrome = snx_arm_itstate(cpu->cpsr);
            *executed += count;
            return SNX_CTR_RUN_FAULT;
        }
        block(cpu);
        ++count;
        if (cpu->exit == SNX_ARM_OK) continue;
        /* the bytes a compiled block was made from changed under it: it stopped before the first changed instruction,
           which compiles anew from where it stopped, and the block is made again the next time it is entered. A
           static block's text is the game's read-only code: one that changed past its first instruction goes on the
           same way, one changed at its entry, which the static table would only enter again, stops */
        if (cpu->exit == SNX_ARM_CODE_CHANGED && process->compile && (compiled || cpu->r[15] != pc)) {
            if (compiled) (void)process->compile(process->compiler, pc, thumb, true);
            cpu->exit = SNX_ARM_OK;
            cpu->syndrome = 0;
            continue;
        }
        if (cpu->exit == SNX_ARM_SVC) {
            uint32_t before[4] = {cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3]}, thread = process->current_thread;
            SnxCtrSvcResult result = snx_ctr_svc(process, cpu->syndrome);
            uint64_t epoch = snx_ctr_space_map_epoch(space);
            if (epoch != map_epoch) {
                memset(g_blocks, 0, sizeof g_blocks);
                map_epoch = g_blocks_epoch = epoch;
            }
            cacheable = !process->aot_stale;
            if (g_settings.ctr_svc_trace && result == SNX_CTR_SVC_RETURN &&
                (g_settings.ctr_svc_trace > 1 || (cpu->r[0] >> 31)))
                fprintf(stderr,
                        "3ds svc trace: thread %u svc 0x%02x at 0x%08x (0x%08x 0x%08x 0x%08x 0x%08x) -> 0x%08x 0x%08x\n",
                        (unsigned)thread, (unsigned)cpu->syndrome, cpu->r[15] - (thumb ? 2u : 4u), before[0], before[1],
                        before[2], before[3], cpu->r[0], cpu->r[1]);
            if (result == SNX_CTR_SVC_EXIT) {
                *executed += count;
                return SNX_CTR_RUN_EXIT;
            }
            if (result == SNX_CTR_SVC_BLOCKED) {
                if (!scheduled(process, &stop, true, &since_poll)) {
                    *executed += count;
                    return stop;
                }
                continue;
            }
            if (result == SNX_CTR_SVC_UNSUPPORTED) {
                *executed += count;
                return SNX_CTR_RUN_UNSUPPORTED;
            }
            cpu->exit = SNX_ARM_OK;
            cpu->syndrome = 0;
            if (!scheduled(process, &stop, true, &since_poll)) {
                *executed += count;
                return stop;
            }
            continue;
        }
        if (cpu->exit == SNX_ARM_BACKEND_UNSUPPORTED || cpu->exit == SNX_ARM_CODE_CHANGED) {
            *executed += count;
            return SNX_CTR_RUN_UNSUPPORTED;
        }
        *executed += count;
        return SNX_CTR_RUN_FAULT;
    }
    *executed += count;
    return SNX_CTR_RUN_BUDGET;
}
