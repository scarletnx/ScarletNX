/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The constant buffer slots bound as dynamic uniform buffers (gpu/vk/cb_dynamic.c), chosen over runs of a game from the
   file of the moves that each run leaves: the choice keeps the last run's slots, those that run did not move too, takes
   others while the device has room for them, and changes the slots only where the best ones move over a quarter more.
   No Vulkan loader, device or GPU is used: the runtime's file calls are the test's */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/cb_dynamic.c"

VkState g_vk;
Settings g_settings;
char g_exename[256] = "game";

static char g_log[4096], g_file[8192];
static size_t g_file_n;                           /* 0: no file */
static u32 g_uniform_slots = VK_CB_DYNAMIC_SLOTS; /* the slots from here on are storage buffers (cbufs_fit_limits) */
void logf_(int level, const char* fmt, ...) {
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vsnprintf(g_log, sizeof g_log, fmt, ap);
    va_end(ap);
}
u8* snx_read_file(const char* path, size_t* size) {
    if (strcmp(path, "/cache/game.vkcbmoves") != 0 || !g_file_n) return NULL;
    u8* p = malloc(g_file_n + 1);
    if (!p) abort();
    memcpy(p, g_file, g_file_n);
    p[g_file_n] = 0;
    *size = g_file_n;
    return p;
}
int snx_write_file(const char* path, const void* data, size_t size) {
    (void)path;
    if (size >= sizeof g_file) return -1;
    memcpy(g_file, data, size);
    g_file[size] = 0;
    g_file_n = size;
    return 0;
}
int plat_replace_file(const char* from, const char* to) {
    (void)from, (void)to;
    return 0;
}
VkDescriptorType vk_cbuf_type(u32 slot) {
    return slot < g_uniform_slots ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

static int g_failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n    log: %s    file:\n%s", __FILE__, __LINE__, #c, g_log, g_file);        \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

typedef struct {
    u32 set, slot;
    u64 moves;
} Move;
typedef struct {
    u32 mask[2], slots;
    u64 through, all; /* the moves of the run, and those through its dynamic slots */
} Run;
/* a run of the game, from its start (the choice) to its end (the file): moves of some slots, none of the others */
static Run run(u32 limit, const Move* moves, u32 n) {
    Run r = {0};
    memset(&g_cb_dynamic, 0, sizeof g_cb_dynamic);
    g_settings.dynamic_constant_buffers = 1;
    g_vk.props.limits.maxDescriptorSetUniformBuffersDynamic = limit;
    vk_cb_dynamic_choose("/cache");
    for (int s = 0; s < 2; s++) {
        r.mask[s] = g_cb_dynamic.mask[s];
        for (u32 bits = r.mask[s]; bits; bits &= bits - 1) r.slots++;
    }
    for (u32 i = 0; i < n; i++) {
        u64 off = 0;
        while (off < moves[i].moves) vk_cb_dynamic_move((int)moves[i].set, moves[i].slot, ++off * 256);
        r.all += moves[i].moves;
        if (r.mask[moves[i].set] >> moves[i].slot & 1) r.through += moves[i].moves;
    }
    vk_cb_dynamic_save();
    CHECK(r.slots <= limit);
    return r;
}
static int same(const Run* a, const Run* b) { return a->mask[0] == b->mask[0] && a->mask[1] == b->mask[1]; }
static void file(const char* lines) { g_file_n = (size_t)snprintf(g_file, sizeof g_file, "%s", lines); }
/* a run that only chooses, and keeps the file as it is */
static Run chosen(u32 limit) {
    char before[sizeof g_file];
    memcpy(before, g_file, sizeof before);
    const Run r = run(limit, NULL, 0);
    CHECK(!strcmp(before, g_file)); /* a run that drew nothing keeps the last run's */
    return r;
}

int main(void) {
    /* a race moves 20 slots, its menus 6 of them */
    Move race[20], menus[6];
    for (u32 i = 0; i < 20; i++) race[i] = (Move){i & 1, i / 2, 1000 - 20 * i};
    for (u32 i = 0; i < 6; i++) menus[i] = (Move){i & 1, i / 2, 600 - 50 * i};
    Run first = run(16, race, 20);
    CHECK(first.slots == 15 && strstr(g_log, "15 of the device's limit 16, from the default order: vertex set"));
    Run second = run(16, race, 20); /* the best: the default order's carried 55% of the moves */
    CHECK(second.slots == 16 && !same(&first, &second) && second.through * 5 >= second.all * 4);
    CHECK(strstr(g_log, "16 of the device's limit 16, from /cache/game.vkcbmoves (the best ones, which move over a "
                        "quarter more than the last run's 15): vertex set"));
    Run third = run(16, menus, 6);
    CHECK(same(&second, &third) && third.through == third.all);
    CHECK(strstr(g_log, "(the last run's 16, and 0 more in the room left)"));
    /* the race after its menus: the slots that the menus did not move are still the last run's */
    Run fourth = run(16, race, 20);
    CHECK(same(&third, &fourth) && fourth.through == second.through);
    Run fifth = run(16, race, 20);
    CHECK(same(&fourth, &fifth));

    /* six slots carry a first run, and ten others move a little from the second on: they take the room the six leave */
    file("");
    Move few[6], many[16];
    for (u32 i = 0; i < 6; i++) few[i] = (Move){i & 1, i / 2, 1000};
    for (u32 i = 0; i < 16; i++) many[i] = (Move){i & 1, i / 2, i < 6 ? 1000 : 120};
    run(16, few, 6);
    run(16, many, 16);
    third = run(16, many, 16);
    CHECK(third.slots == 16 && third.through * 100 >= third.all * 94);
    fourth = run(16, many, 16);
    CHECK(same(&third, &fourth));

    /* a file of a build that wrote no line for a slot that did not move: six were dynamic, ten others moved */
    file("snx-cbmoves 1\n0 0 1000 1\n0 1 1000 1\n0 2 1000 1\n1 0 1000 1\n1 1 1000 1\n1 2 1000 1\n0 3 120 0\n0 4 120 0\n"
         "0 5 120 0\n0 6 120 0\n0 7 120 0\n1 3 120 0\n1 4 120 0\n1 5 120 0\n1 6 120 0\n1 7 120 0\n");
    first = chosen(16);
    CHECK(first.mask[0] == 0xff && first.mask[1] == 0xff);
    CHECK(strstr(g_log, "16 of the device's limit 16") &&
          strstr(g_log, "(the last run's 6, and 10 more in the room left)"));
    /* with room for two more, the most moved of the others; the last run's six stay */
    file("snx-cbmoves 1\n0 0 1000 1\n0 1 1000 1\n0 2 1000 1\n1 0 1000 1\n1 1 1000 1\n1 2 1000 1\n0 3 120 0\n0 4 121 0\n"
         "1 3 122 0\n");
    first = chosen(8);
    CHECK(first.mask[0] == 0x17 && first.mask[1] == 0xf);
    CHECK(strstr(g_log, "(the last run's 6, and 2 more in the room left)"));

    /* the last run's slots stay while the best move no more than a quarter more than they do, and go past that */
    file("snx-cbmoves 1\n0 0 125 0\n0 1 100 1\n");
    first = chosen(1);
    CHECK(first.mask[0] == 2 && strstr(g_log, "(the last run's 1, and 0 more in the room left)"));
    file("snx-cbmoves 1\n0 0 126 0\n0 1 100 1\n");
    first = chosen(1);
    CHECK(first.mask[0] == 1 && strstr(g_log, "(the best ones, which move over a quarter more than the last run's 1)"));
    /* none dynamic in the last run (a device whose limit was 0, say) */
    file("snx-cbmoves 1\n0 0 125 0\n0 1 100 0\n");
    first = chosen(1);
    CHECK(first.mask[0] == 1 && strstr(g_log, "(the best ones: the last run had none)"));

    /* no slot past the uniform buffers the device takes, whatever the file says of it */
    g_uniform_slots = 2;
    file("snx-cbmoves 1\n0 0 10 1\n0 1 5 0\n0 2 100 1\n0 3 90 0\n1 17 0 1\n");
    first = chosen(16);
    CHECK(first.mask[0] == 3 && !first.mask[1]);
    g_uniform_slots = VK_CB_DYNAMIC_SLOTS;

    /* every slot dynamic, or moving: the file's text holds a line for each */
    Move all[2 * VK_CB_DYNAMIC_SLOTS];
    for (u32 i = 0; i < 2 * VK_CB_DYNAMIC_SLOTS; i++) all[i] = (Move){i & 1, i / 2, 100 + i};
    file("");
    run(2 * VK_CB_DYNAMIC_SLOTS, all, 2 * VK_CB_DYNAMIC_SLOTS);
    first = run(2 * VK_CB_DYNAMIC_SLOTS, all, 2 * VK_CB_DYNAMIC_SLOTS);
    CHECK(first.slots == 2 * VK_CB_DYNAMIC_SLOTS && first.through == first.all);

    if (g_failures) {
        fprintf(stderr, "vk_cb_dynamic: %d failed\n", g_failures);
        return 1;
    }
    puts("vk_cb_dynamic: the slots kept, filled and changed from one run to the next");
    return 0;
}
