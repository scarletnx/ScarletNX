/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/connect.h"
#include <stdio.h>
#include <string.h>

static struct {
    char who[SNX_CONNECT_PLAYERS][64]; /* the controller holding each player, or kept for; "" none */
    u64 left[SNX_CONNECT_PLAYERS];     /* when it left, 0 while it holds the player */
    unsigned waiting;                  /* the players a pause waits for, a bit each */
    unsigned paused;                   /* the players whose leaving paused the game once: a pause each */
    int plus;                          /* + held on a controller while a pause lasts */
} g_connect;

void connect_reset(void) { memset(&g_connect, 0, sizeof g_connect); }
int connect_kept(int p, u64 now_ns) { /* a pause waiting for it keeps it past the time */
    return p >= 0 && p < SNX_CONNECT_PLAYERS && g_connect.who[p][0] && g_connect.left[p] &&
           (now_ns - g_connect.left[p] <= SNX_CONNECT_HOLD_NS || (g_connect.waiting >> p & 1));
}
int connect_join(const char* who, unsigned busy, u64 now_ns) {
    int p, take = -1;
    for (p = 0; p < SNX_CONNECT_PLAYERS && take < 0; ++p) /* its own player, kept for it */
        if (!(busy >> p & 1) && connect_kept(p, now_ns) && !strcmp(g_connect.who[p], who)) take = p;
    for (p = 0; p < SNX_CONNECT_PLAYERS && take < 0; ++p)
        if (!(busy >> p & 1) && !connect_kept(p, now_ns)) take = p;
    if (take < 0) return -1;
    snprintf(g_connect.who[take], sizeof g_connect.who[take], "%s", who);
    g_connect.left[take] = 0;
    g_connect.paused &= ~(1u << take);
    return take;
}
void connect_held(int p, const char* who) {
    if (p < 0 || p >= SNX_CONNECT_PLAYERS) return;
    snprintf(g_connect.who[p], sizeof g_connect.who[p], "%s", who);
    g_connect.left[p] = 0;
}
void connect_leave(int p, u64 now_ns) {
    if (p >= 0 && p < SNX_CONNECT_PLAYERS && g_connect.who[p][0]) g_connect.left[p] = now_ns ? now_ns : 1;
}
/* the players of bits as P1, P3 */
static void names(unsigned bits, char* out, size_t n) {
    int p;
    size_t at = 0;
    if (n) out[0] = 0;
    for (p = 0; p < SNX_CONNECT_PLAYERS && at < n; ++p)
        if (bits >> p & 1) {
            int w = snprintf(out + at, n - at, "%sP%d", at ? ", " : "", p + 1);
            at += w > 0 ? (size_t)w : 0;
        }
}
int connect_tick(u64 now_ns, int running, int pads, int plus, char* notice, size_t n, unsigned* freed) {
    unsigned gone = 0;
    char who[64];
    int p;
    *freed = 0;
    if (n) notice[0] = 0;
    for (p = 0; p < SNX_CONNECT_PLAYERS; ++p) {
        if (!g_connect.left[p]) continue;
        if (now_ns - g_connect.left[p] > SNX_CONNECT_HOLD_NS && !(g_connect.waiting >> p & 1)) {
            g_connect.who[p][0] = 0; /* a pause waiting for it keeps it: its controller is how the game goes on */
            g_connect.left[p] = 0;
            g_connect.paused &= ~(1u << p);
            *freed |= 1u << p;
        } else if (now_ns - g_connect.left[p] >= SNX_CONNECT_PAUSE_NS && !(g_connect.paused >> p & 1))
            gone |= 1u << p;
    }
    if (g_connect.waiting) {
        unsigned back = 0;
        for (p = 0; p < SNX_CONNECT_PLAYERS; ++p)
            if ((g_connect.waiting >> p & 1) && !g_connect.left[p] && g_connect.who[p][0]) back |= 1u << p;
        if (back == g_connect.waiting) {
            g_connect.waiting = 0;
            g_connect.plus = 0;
            return CONNECT_RESUME;
        }
        if (plus) /* + counts once let go, so the game never sees it on resuming */
            g_connect.plus = 1;
        else if (g_connect.plus) {
            names(g_connect.waiting, who, sizeof who);
            for (p = 0; p < SNX_CONNECT_PLAYERS; ++p)
                if (g_connect.waiting >> p & 1 && g_connect.left[p]) { /* no longer kept for its controller */
                    g_connect.who[p][0] = 0;
                    g_connect.left[p] = 0;
                    *freed |= 1u << p;
                }
            g_connect.waiting = 0;
            g_connect.plus = 0;
            snprintf(notice, n, "Going on without %s", who);
            return CONNECT_RESUME;
        }
        names(g_connect.waiting, who, sizeof who);
        if (pads)
            snprintf(notice, n, "%s disconnected: reconnect, or press + on another controller to go on", who);
        else
            snprintf(notice, n, "%s disconnected: reconnect to go on", who);
        return CONNECT_NONE;
    }
    if (!gone || !running) return CONNECT_NONE;
    g_connect.waiting = gone;
    g_connect.paused |= gone; /* each leave pauses once */
    names(gone, who, sizeof who);
    snprintf(notice, n,
             pads ? "%s disconnected: reconnect, or press + on another controller to go on"
                  : "%s disconnected: reconnect to go on",
             who);
    return CONNECT_PAUSE;
}
