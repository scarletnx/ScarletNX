/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ARM9's four timers. A timer that counts the bus clock is not stepped: its count is the cycles since it was last
   set, and its overflow an event at the cycle it comes. One that counts the timer below it is stepped by that one's
   overflows. */
#include "nds/board/nds.h"

static unsigned prescale(const NdsTimer* t) {
    static const unsigned char shift[4] = {0, 6, 8, 10};
    return shift[t->control & 3];
}
static bool runs(const NdsTimer* t) { return (t->control & 0x80) != 0; }
static bool counts_up(unsigned index, const NdsTimer* t) { return index && (t->control & 4); }

/* the count now, and since the cycle its last whole tick came at */
static void sync(NdsBoard* board, unsigned index) {
    NdsTimer* t = &board->timer[index];
    if (!runs(t) || counts_up(index, t)) {
        t->since = board->cycles;
        return;
    }
    uint64_t ticks = (board->cycles - t->since) >> prescale(t);
    t->counter = (uint32_t)((t->counter + ticks) & 0xFFFF);
    t->since += ticks << prescale(t);
}

static void arm(NdsBoard* board, unsigned index) {
    NdsTimer* t = &board->timer[index];
    uint64_t at = UINT64_MAX;
    if (runs(t) && !counts_up(index, t)) at = t->since + ((uint64_t)(0x10000u - t->counter) << prescale(t));
    nds_schedule(board, (NdsEvent)(NDS_EVENT_TIMER0 + index), at);
}

uint16_t nds_timer_read(NdsBoard* board, unsigned index) {
    sync(board, index);
    return (uint16_t)board->timer[index].counter;
}

void nds_timer_write(NdsBoard* board, unsigned index, uint16_t reload, uint16_t control, unsigned which) {
    NdsTimer* t = &board->timer[index];
    sync(board, index);
    if (which & 1) t->reload = reload;
    if (which & 2) {
        bool ran = runs(t);
        t->control = control & 0xC7;
        if (!ran && runs(t)) t->counter = t->reload;
        t->since = board->cycles;
    }
    arm(board, index);
}

static void overflow(NdsBoard* board, unsigned index) {
    NdsTimer* t = &board->timer[index];
    t->counter = t->reload;
    if (t->control & 0x40) nds_irq(board, NDS_IRQ_TIMER0 + index);
    if (index < 3) {
        NdsTimer* up = &board->timer[index + 1];
        if (runs(up) && (up->control & 4) && ++up->counter > 0xFFFF) overflow(board, index + 1);
    }
}

void nds_timer_event(NdsBoard* board, unsigned index) {
    NdsTimer* t = &board->timer[index];
    t->since = board->events[NDS_EVENT_TIMER0 + index];
    overflow(board, index);
    arm(board, index);
}
