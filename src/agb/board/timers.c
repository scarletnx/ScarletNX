/* SPDX-License-Identifier: GPL-3.0-or-later AND BSD-2-Clause
   Copyright 2026 the ScarletNX contributors
   Copyright 2026 Mireille Meyer, Gloria Goertz */
/* the AGB timers: four 16 bit counters at the CPU clock, the first two of them feeding the sound FIFOs. When a write
   lands and where a started timer begins counting follow NanoBoyAdvance (its Timer.cc, commit 2da64af), altered from
   its original form into C:

   Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
   following conditions are met:

   1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
      following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
      following disclaimer in the documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */
#include "agb/board/board.h"

#define AGB_TIMER_ENABLE  0x0080
#define AGB_TIMER_IRQ     0x0040
#define AGB_TIMER_CASCADE 0x0004

static unsigned shift_of(uint16_t control) {
    static const uint8_t shifts[4] = {0, 6, 8, 10}; /* the prescaler: 1, 64, 256 or 1024 cycles a count */
    return shifts[control & 3];
}

/* a timer that counts the clock: on, and not counting the timer before it (timer 0 has none, so its bit does nothing) */
static int clocked(const AgbBoard* board, int index) {
    uint16_t control = board->timer[index].control;
    return (control & AGB_TIMER_ENABLE) && !(index && (control & AGB_TIMER_CASCADE));
}

/* its count at the cycle at: what it held when it started, and a tick per prescaler period since. A read the cycle
   before it starts gives one less */
static uint32_t count_at(const AgbTimer* timer, uint64_t at) {
    int64_t ticks = (int64_t)(at - timer->since) >> shift_of(timer->control);
    return timer->counter + (uint32_t)ticks;
}

static uint64_t overflow_at(const AgbTimer* timer) {
    return timer->since + ((uint64_t)(0x10000u - timer->counter) << shift_of(timer->control));
}

/* the count passed 0xFFFF at the cycle at: the reload, the FIFO's sample, the interrupt, a tick of the next timer when it
   counts this one's */
static void overflow(AgbBoard* board, int index, uint64_t at) {
    AgbTimer* timer = &board->timer[index];
    timer->counter = timer->reload;
    timer->since = at;
    if (index < 2) agb_apu_timer_overflow(board, index, at); /* the sound FIFOs play at timer 0's or 1's rate */
    if (timer->control & AGB_TIMER_IRQ) agb_irq_raise(board, AGB_IRQ_TIMER(index), at);
    if (index < 3 && (board->timer[index + 1].control & AGB_TIMER_ENABLE) &&
        (board->timer[index + 1].control & AGB_TIMER_CASCADE)) {
        AgbTimer* next = &board->timer[index + 1];
        if (++next->counter >= 0x10000) overflow(board, index + 1, at);
    }
}

/* TMxCNT_H lands at the cycle at. A timer started takes its reload a cycle later, the prescaler's phase the board's
   clock: the CPU's store at t gives it its first count at t + 2 at a prescaler of 1. One that overflows in that cycle
   (it held 0xFFFF on the prescaler's edge) keeps counting from there */
static void control_lands(AgbBoard* board, int index, uint64_t at) {
    AgbTimer* timer = &board->timer[index];
    int was = (timer->control & AGB_TIMER_ENABLE) != 0;
    if (clocked(board, index)) {
        timer->counter = count_at(timer, at);
        if (timer->counter >= 0x10000) overflow(board, index, at);
    }
    timer->control = timer->next_control & 0x00C7;
    timer->control_at = 0;
    if (!(timer->control & AGB_TIMER_ENABLE)) return;
    uint64_t phase = at & ((1u << shift_of(timer->control)) - 1);
    int cascade = index && (timer->control & AGB_TIMER_CASCADE);
    if (was) {
        if (!cascade) timer->since = at - phase;
    } else if (cascade)
        timer->counter = timer->reload;
    else if (timer->counter == 0xFFFF && !phase)
        timer->since = at;
    else {
        timer->counter = timer->reload;
        timer->since = at - phase + 1;
    }
}

void agb_timer_write(AgbBoard* board, uint32_t offset, uint16_t value) {
    int index = (int)((offset - 0x100) >> 2), part = (int)(offset & 3);
    if (index < 0 || index > 3) return;
    AgbTimer* timer = &board->timer[index];
    if (!part) {
        timer->next_reload = value;
        timer->reload_at = board->now + 1;
    } else {
        timer->next_control = value;
        timer->control_at = board->now + 1;
    }
}

uint16_t agb_timer_written(AgbBoard* board, uint32_t offset) {
    int index = (int)((offset - 0x100) >> 2);
    if (index < 0 || index > 3) return 0;
    const AgbTimer* timer = &board->timer[index];
    if (offset & 3) return timer->control_at ? timer->next_control : timer->control;
    return timer->reload_at ? timer->next_reload : timer->reload;
}

uint16_t agb_timer_read(AgbBoard* board, uint32_t offset) {
    int index = (int)((offset - 0x100) >> 2), part = (int)(offset & 3);
    if (index < 0 || index > 3) return 0;
    const AgbTimer* timer = &board->timer[index];
    if (part) return timer->control;
    return (uint16_t)(clocked(board, index) ? count_at(timer, board->now) : timer->counter);
}

/* the next cycle timer index may overflow: a clocked one's own, a counting one's no earlier than the timer before it */
static uint64_t next_overflow(const AgbBoard* board, int index) {
    const AgbTimer* timer = &board->timer[index];
    uint64_t at = timer->control_at ? timer->control_at : UINT64_MAX;
    if (timer->control & AGB_TIMER_ENABLE) {
        uint64_t own = clocked(board, index) ? overflow_at(timer)
                       : index               ? next_overflow(board, index - 1)
                                             : UINT64_MAX;
        if (own < at) at = own;
    }
    return at;
}

uint64_t agb_timer_next_irq(const AgbBoard* board) {
    uint64_t at = UINT64_MAX;
    for (int index = 0; index < 4; ++index) {
        const AgbTimer* timer = &board->timer[index];
        if (!(board->irq_ie & AGB_IRQ_TIMER(index))) continue;
        if (!(timer->control & AGB_TIMER_IRQ) && !(timer->control_at && (timer->next_control & AGB_TIMER_IRQ)))
            continue;
        uint64_t own = next_overflow(board, index);
        if (own < at) at = own;
    }
    return at;
}

void agb_timer_step(AgbBoard* board, uint32_t cycles) {
    board->now += cycles;
    for (;;) {
        /* the first event up to now; at one cycle an overflow comes first, then a reload's write, then a control's */
        uint64_t first = 0;
        int which = -1, kind = 0;
        for (int index = 0; index < 4; ++index) {
            const AgbTimer* timer = &board->timer[index];
            uint64_t at[3] = {clocked(board, index) ? overflow_at(timer) : 0, timer->reload_at, timer->control_at};
            for (int k = 0; k < 3; ++k)
                if (at[k] && at[k] <= board->now && (which < 0 || at[k] < first || (at[k] == first && k < kind))) {
                    first = at[k];
                    which = index;
                    kind = k;
                }
        }
        if (which < 0) return;
        AgbTimer* timer = &board->timer[which];
        if (kind == 0)
            overflow(board, which, first);
        else if (kind == 1) {
            timer->reload = timer->next_reload;
            timer->reload_at = 0;
        } else
            control_lands(board, which, first);
    }
}
