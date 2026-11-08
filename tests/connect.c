/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Controllers taking players as a console's do (runtime/platform/connect.c, gamepad_assign=1): the lowest free player,
   one kept 45 s for its controller and taken back, the pause after 3 s and its notice, the controller back or + let go
   on another to go on, a pause once per leave, none while the game is paused, a ninth that waits */
#include "nx/runtime/platform/connect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define S 1000000000ull
static struct {
    char notice[128]; /* the last tick's */
    unsigned freed;
} g_test;
#define notice g_test.notice
#define freed  g_test.freed
static int tick(u64 t, int running, int pads, int plus) {
    return connect_tick(t, running, pads, plus, notice, sizeof notice, &freed);
}

int main(void) {
    u64 t = 100 * S;
    int p;
    connect_reset();
    /* in the order they come, the lowest free */
    assert(connect_join("serial:a", 0, t) == 0 && connect_join("serial:b", 1, t) == 1);
    /* B leaves: P2 kept for it, a third takes P3, B comes back to P2 */
    connect_leave(1, t);
    assert(connect_kept(1, t + S) && !connect_kept(0, t + S));
    assert(connect_join("guid:c", 1, t + S) == 2);
    assert(tick(t + 2 * S, 1, 2, 0) == CONNECT_NONE && !notice[0]); /* under 3 s: no pause yet */
    assert(connect_join("serial:b", 1 | 4, t + 2 * S) == 1 && !connect_kept(1, t + 2 * S));
    assert(tick(t + 10 * S, 1, 3, 0) == CONNECT_NONE); /* back in time: no pause */
    /* gone 3 s while the game runs: paused, said, and said again while it lasts; back: resumed */
    t += 20 * S;
    connect_leave(1, t);
    assert(tick(t + 2 * S, 1, 2, 0) == CONNECT_NONE);
    assert(tick(t + 3 * S, 1, 2, 0) == CONNECT_PAUSE);
    assert(!strcmp(notice, "P2 disconnected: reconnect, or press + on another controller to go on"));
    assert(tick(t + 4 * S, 0, 2, 0) == CONNECT_NONE && !strcmp(notice, "P2 disconnected: reconnect, or press + on "
                                                                       "another controller to go on"));
    assert(tick(t + 60 * S, 0, 2, 0) == CONNECT_NONE && !freed && connect_kept(1, t + 4 * S)); /* kept past 45 s */
    assert(connect_join("serial:b", 1 | 4, t + 61 * S) == 1);
    assert(tick(t + 61 * S, 0, 3, 0) == CONNECT_RESUME && !notice[0]);
    /* gone again: a pause again; + held and let go on another: on without it, its player no longer kept */
    t += 100 * S;
    connect_leave(1, t);
    assert(tick(t + 3 * S, 1, 2, 0) == CONNECT_PAUSE);
    assert(tick(t + 4 * S, 0, 2, 1) == CONNECT_NONE); /* held: the game must not see it */
    assert(tick(t + 5 * S, 0, 2, 0) == CONNECT_RESUME && !strcmp(notice, "Going on without P2") && freed == 2);
    assert(!connect_kept(1, t + 5 * S) && connect_join("guid:d", 1 | 4, t + 6 * S) == 1);
    /* the game paused already when the 3 s pass: no pause then; once it runs, one */
    t += 100 * S;
    connect_leave(2, t);
    assert(tick(t + 4 * S, 0, 2, 0) == CONNECT_NONE && !notice[0]);
    assert(tick(t + 5 * S, 1, 2, 0) == CONNECT_PAUSE && !strcmp(notice, "P3 disconnected: reconnect, or press + on "
                                                                        "another controller to go on"));
    /* the last one gone: the notice says only reconnect */
    connect_leave(0, t + 5 * S);
    connect_leave(1, t + 5 * S);
    assert(tick(t + 6 * S, 0, 0, 0) == CONNECT_NONE && !strcmp(notice, "P3 disconnected: reconnect to go on"));
    assert(connect_join("guid:c", 0, t + 7 * S) == 2 && tick(t + 7 * S, 0, 1, 0) == CONNECT_RESUME);
    /* P1 and P2 gone 3 s at once: one pause for both */
    assert(tick(t + 8 * S, 1, 1, 0) == CONNECT_PAUSE && !strcmp(notice, "P1, P2 disconnected: reconnect, or press + on "
                                                                        "another controller to go on"));
    assert(tick(t + 9 * S, 0, 1, 1) == CONNECT_NONE && tick(t + 9 * S, 0, 1, 0) == CONNECT_RESUME);
    assert(!strcmp(notice, "Going on without P1, P2") && freed == 3);
    /* kept 45 s with the game paused by its player all along: then free, said in freed */
    connect_reset();
    assert(connect_join("serial:e", 0, t) == 0);
    connect_leave(0, t);
    assert(tick(t + 45 * S, 0, 0, 0) == CONNECT_NONE && !freed && connect_kept(0, t + 45 * S));
    assert(tick(t + 46 * S, 0, 0, 0) == CONNECT_NONE && freed == 1 && !connect_kept(0, t + 46 * S));
    /* every player taken or kept: a ninth waits */
    connect_reset();
    for (p = 0; p < 8; ++p) {
        char who[16];
        snprintf(who, sizeof who, "guid:%d", p);
        assert(connect_join(who, (1u << p) - 1, t) == p);
    }
    assert(connect_join("guid:9", 0xFF, t) == -1);
    connect_leave(7, t);
    assert(connect_join("guid:9", 0x7F, t + S) == -1 && connect_join("guid:7", 0x7F, t + S) == 7);
    /* a player its controller held when the mode came on is kept for it like the others */
    connect_reset();
    connect_held(3, "serial:f");
    connect_leave(3, t);
    assert(connect_kept(3, t + S) && connect_join("serial:g", 0, t + S) == 0 &&
           connect_join("serial:f", 1, t + S) == 3);
    puts("connect: players by order, kept and taken back, the pause, its notice, + to go on, and a ninth passed");
    return 0;
}
