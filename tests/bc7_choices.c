/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 encoder's choices (gpu/vk/bc7_choices.c), without a GPU: a level's words are made zero, a strip's come back
   at its place, and a level of another count under the same key starts over. The game's file written at the exit
   gives the same words back in the next run; a file of another encoder's version, or one that ends inside a level,
   is said with both values, and in the second the levels before it are read. A run that kept none leaves the last
   run's file as it was */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nx/gpu/vk/bc7_choices.c"

static char g_said[2048]; /* the lines logged, in turn */
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    char line[1024];
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    strncat(g_said, line, sizeof g_said - strlen(g_said) - 1);
}

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* every level kept let go, as a new run starts with none */
static void forget_all(void) {
    for (u32 i = 0; i < g_bc7_choices.cap; i++) free(g_bc7_choices.map[i].words);
    free(g_bc7_choices.map);
    g_bc7_choices.map = NULL;
    g_bc7_choices.cap = g_bc7_choices.used = 0;
    g_bc7_choices.bytes = 0;
}

int main(void) {
    char dir[] = "/tmp/snx-bc7-choices-XXXXXX", path[256], tmp[300];
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/game.bc7choice", dir);
    const u64 a[2] = {1, 2}, b[2] = {3, 4};
    /* a level's words start zero; a strip's land at its place */
    u16* w = vk_bc7_choices_take(a, 100);
    CHECK(w && !w[0] && !w[99] && vk_bc7_choices_find(a, 100) == w && !vk_bc7_choices_find(b, 100));
    const u32 got[3] = {0x1006u, 0x3F1u, 0xA05u}; /* bits past the twelfth are not a choice's */
    vk_bc7_choices_put(a, 100, 40, got, 3);
    CHECK(w[39] == 0 && w[40] == 6 && w[41] == 0x3F1 && w[42] == 0xA05 && w[43] == 0);
    vk_bc7_choices_put(a, 100, 99, got, 3); /* past the level's end: nothing */
    CHECK(w[99] == 0);
    u16* w2 = vk_bc7_choices_take(b, 7);
    CHECK(w2);
    w2[6] = 0x123;
    /* another count under a key: another level, made again */
    CHECK(!vk_bc7_choices_find(a, 50));
    /* written at the exit, read as the next run starts: the same words */
    CHECK(vk_bc7_choices_load(path) == 0); /* no file yet: the first run */
    vk_bc7_choices_save();
    forget_all();
    CHECK(!vk_bc7_choices_find(a, 100));
    g_said[0] = 0;
    CHECK(vk_bc7_choices_load(path) == 2);
    CHECK(strstr(g_said, "choices of 2 levels (107 blocks"));
    w = vk_bc7_choices_find(a, 100);
    w2 = vk_bc7_choices_find(b, 7);
    CHECK(w && w2 && w[40] == 6 && w[41] == 0x3F1 && w[42] == 0xA05 && !w[0] && w2[6] == 0x123);
    /* a run that kept none leaves the file as it was */
    forget_all();
    vk_bc7_choices_save();
    CHECK(vk_bc7_choices_load(path) == 2);
    /* a file of another encoder's version: not read, said with both */
    FILE* f = fopen(path, "r+b");
    CHECK(f);
    u64 other = VK_BC7_ENCODER_VERSION + 7;
    CHECK(fseek(f, 16, SEEK_SET) == 0 && fwrite(&other, 8, 1, f) == 1 && fclose(f) == 0);
    forget_all();
    g_said[0] = 0;
    CHECK(vk_bc7_choices_load(path) == 0 && !g_bc7_choices.used);
    char want[200];
    snprintf(want, sizeof want, "of BC7 encoder %llu, not this build's \"%s\" of encoder %u", (unsigned long long)other,
             BC7_CHOICES_MAGIC, (unsigned)VK_BC7_ENCODER_VERSION);
    CHECK(strstr(g_said, want));
    /* a file that ends inside its second level: the first is read, and the line says where it ends */
    forget_all();
    w = vk_bc7_choices_take(a, 100);
    w[5] = 6;
    w2 = vk_bc7_choices_take(b, 7);
    vk_bc7_choices_save();
    snprintf(tmp, sizeof tmp, "%s", path);
    CHECK(truncate(tmp, 32 + 24 + 200 + 24 + 4) == 0);
    forget_all();
    g_said[0] = 0;
    CHECK(vk_bc7_choices_load(path) == 1 && g_bc7_choices.used == 1);
    CHECK(strstr(g_said, "ends inside its level 1 of 2 (7 blocks, 4 bytes left)"));
    remove(path);
    rmdir(dir);
    printf("PASS bc7_choices: %u checks\n", checks);
    return 0;
}
