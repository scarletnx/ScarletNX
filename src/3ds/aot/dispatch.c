/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/aot/ctr_discover.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/kernel/ctr_kernel.h"
#include "3ds/runtime/internal.h"
#include "3ds/memory/host.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned snx_ctr_module_memory(void) { return ctr_host_flat() ? SNX_CTR_MODULE_FLAT_MODE : SNX_CTR_MODULE_CHECKED_MODE; }
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
   dispatch again; no thread ever running again is BLOCKED. */
static bool scheduled(SnxCtrProcess* process, SnxCtrRunResult* stop) {
    SnxCtrSchedule s = snx_ctr_schedule(process);
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
SnxCtrRunResult snx_ctr_dispatch(const SnxCtrModule* m, SnxCtrProcess* process, uint64_t limit, uint64_t* executed) {
    if (!executed) return SNX_CTR_RUN_FAULT;
    *executed = 0;
    SnxArmCPU* cpu = snx_ctr_process_cpu(process);
    if (!cpu || !m) return SNX_CTR_RUN_FAULT;
    SnxCtrRunResult stop;
    if (!scheduled(process, &stop)) return stop;
    uint32_t since_poll = 0;
    while (*executed < limit) {
        if (process->reclaim) process->reclaim(process->compiler);
        /* Every block returns here, so a loop that makes no SVC still reaches the scheduler: due vblanks, audio frames,
           timers and timeouts are delivered, and a thread they wake with a better priority takes the core, as the
           interrupt would preempt it on the console */
        if (++since_poll == SNX_CTR_POLL_BLOCKS) {
            since_poll = 0;
            if (!scheduled(process, &stop)) return stop;
        }
        uint32_t pc = cpu->r[15];
        bool thumb = !!(cpu->cpsr & SNX_ARM_T);
        unsigned width = thumb ? 2 : 4;
        if (pc & (width - 1)) {
            cpu->exit = SNX_ARM_ALIGNMENT;
            cpu->syndrome = pc;
            return SNX_CTR_RUN_FAULT;
        }
        unsigned char instruction[4];
        if (snx_ctr_fetch(snx_ctr_process_space(process), pc, instruction, width) != SNX_CTR_MEM_OK) {
            cpu->exit = SNX_ARM_PREFETCH_ABORT;
            cpu->syndrome = pc;
            return SNX_CTR_RUN_FAULT;
        }
        SnxArmBlock block = NULL;
        if (pc >= m->text_address && (uint64_t)pc + width <= (uint64_t)m->text_address + m->text_bytes &&
            !window_marked(process, pc - m->text_address)) {
            uint32_t index = (pc - m->text_address) / width;
            block = thumb ? m->thumb_blocks[index] : m->arm_blocks[index];
        }
        /* code the static discovery didn't reach (an indirect target, a module loaded at run time) compiles now */
        bool compiled = !block && process->compile;
        if (compiled) block = process->compile(process->compiler, pc, thumb, false);
        if (!block) {
            cpu->exit = SNX_ARM_BACKEND_UNSUPPORTED;
            cpu->syndrome = pc;
            return SNX_CTR_RUN_UNSUPPORTED;
        }
        block(cpu);
        ++*executed;
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
            if (g_settings.ctr_svc_trace && result == SNX_CTR_SVC_RETURN &&
                (g_settings.ctr_svc_trace > 1 || (cpu->r[0] >> 31)))
                fprintf(stderr,
                        "3ds svc trace: thread %u svc 0x%02x at 0x%08x (0x%08x 0x%08x 0x%08x 0x%08x) -> 0x%08x 0x%08x\n",
                        (unsigned)thread, (unsigned)cpu->syndrome, cpu->r[15] - (thumb ? 2u : 4u), before[0], before[1],
                        before[2], before[3], cpu->r[0], cpu->r[1]);
            if (result == SNX_CTR_SVC_EXIT) return SNX_CTR_RUN_EXIT;
            if (result == SNX_CTR_SVC_BLOCKED) {
                if (!scheduled(process, &stop)) return stop;
                continue;
            }
            if (result == SNX_CTR_SVC_UNSUPPORTED) return SNX_CTR_RUN_UNSUPPORTED;
            cpu->exit = SNX_ARM_OK;
            cpu->syndrome = 0;
            if (!scheduled(process, &stop)) return stop;
            continue;
        }
        if (cpu->exit == SNX_ARM_BACKEND_UNSUPPORTED || cpu->exit == SNX_ARM_CODE_CHANGED)
            return SNX_CTR_RUN_UNSUPPORTED;
        return SNX_CTR_RUN_FAULT;
    }
    return SNX_CTR_RUN_BUDGET;
}
