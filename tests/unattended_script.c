/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* --script's presses against a clock and a present count the test sets: one at a time in ms is held press_len ms from
   then, one at present N ("Nf") from the first ask at or after that present, however late it comes, and both mix.
   Then input_record and input_replay on the backend's polls at the presents the test sets, with the game's folder the
   one ctest gives: what a record writes in its slot and what it says of the one it replaces, what a replay gives and
   when it ends, a record of a replay, and each slot and line refused */
#define plat_ns test_ns
#include "nx/runtime/platform/unattended.c"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static u64 g_now;
u64 test_ns(void) { return g_now; }
int g_frames;
char g_exedir[1024], g_exename[256]; /* the game's folder and name, the slots' */
void snx_save_before_exit(void) {}
void ini_poll(int final) { (void) final; }
void plat_exit_now(int code) { exit(code); }
static char g_log[4096]; /* the log's lines since the test last cleared it */
void logf_(int lvl, const char* fmt, ...) {
    char line[1024];
    size_t n = strlen(g_log);
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stderr);
    snprintf(g_log + n, sizeof g_log - n, "%s", line);
}
const char* snx_build_info(void) { return "ScarletNX test"; }
/* fatal's message, and back to where refused asked */
static jmp_buf g_fatal_at;
static char g_fatal[1024];
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fatal, sizeof g_fatal, fmt, ap);
    va_end(ap);
    longjmp(g_fatal_at, 1);
}

static unsigned checks, failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* the buttons at ms after the start, with frames presented */
static u64 at(u64 ms, int frames) {
    g_now = 1000000000ull + ms * 1000000ull;
    g_frames = frames;
    return unattended_buttons();
}

/* ---- input_record and input_replay ---- */

/* the message fatal ends the run with when key takes value, "" when the value is taken */
static const char* refused(const char* key, const char* value) {
    g_fatal[0] = 0;
    if (!setjmp(g_fatal_at)) unattended_ini(key, value);
    return g_fatal;
}

/* a backend's poll at present frame: the controls' input, and what unattended_pad left of it */
static u64 g_b;
static i32 g_st[4];
static void poll(int frame, u64 b, i32 lx, i32 ly, i32 rx, i32 ry) {
    g_frames = frame;
    g_b = b;
    g_st[0] = lx;
    g_st[1] = ly;
    g_st[2] = rx;
    g_st[3] = ry;
    unattended_pad(&g_b, g_st);
}
static int gave(u64 b, i32 lx, i32 ly, i32 rx, i32 ry) {
    return g_b == b && g_st[0] == lx && g_st[1] == ly && g_st[2] == rx && g_st[3] == ry;
}
/* what poll gave, a format and its arguments: last in a CHECK, as no argument can follow them */
#define GAVE "%llx:%d,%d,%d,%d", (unsigned long long)g_b, g_st[0], g_st[1], g_st[2], g_st[3]

static void put(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    int ok = f && fputs(text, f) >= 0;
    if (f) ok &= !fclose(f);
    CHECK(ok, "%s not written", path);
}
static char g_text[4096];
/* the file's text, NULL when there is no file */
static const char* got(const char* path) {
    FILE* f = fopen(path, "rb");
    size_t n;
    if (!f) return NULL;
    n = fread(g_text, 1, sizeof g_text - 1, f);
    g_text[n] = 0;
    fclose(f);
    return g_text;
}
static int same(const char* path, const char* text) {
    const char* t = got(path);
    return t && !strcmp(t, text);
}
/* a slot's file as the game's folder and name make it: <game folder>/tas/<name>.<slot>.snxtas */
static const char* slot_file(int slot) {
    static char path[8][1400];
    snprintf(path[slot % 8], sizeof path[0], "%stas/%s.%d.snxtas", g_exedir, g_exename, slot);
    return path[slot % 8];
}

static void tas(const char* dir) {
    static const char header[] = "# input_record of ScarletNX test\n"
                                 "# frame:buttons:lx,ly,rx,ry each present player 1's input changed: frame the "
                                 "presents since the game's start, buttons in hex\n";
    static const char lines[] = "3:1:0,0,0,0\n"
                                "4:3:0,0,0,0\n"
                                "5:3:-32767,100,0,32767\n"
                                "7:8000:0,0,0,0\n"
                                "8:0:0,0,0,0\n";
    static const struct {
        const char *text, *says;
    } bad[] = {
        {"5:1:0,0,0\n", "input_replay: slot 4, line 1 \"5:1:0,0,0\": it is not frame:buttons:lx,ly,rx,ry"},
        {"# a note\n\n5:1:0,0,0,0,0\n", "input_replay: slot 4, line 3 \"5:1:0,0,0,0,0\": it is not frame:buttons"},
        {"5;1:0,0,0,0\n", "line 1 \"5;1:0,0,0,0\": it is not frame:buttons:lx,ly,rx,ry"},
        {"x:1:0,0,0,0\n", "line 1 \"x:1:0,0,0,0\": its frame \"x\" is not a whole number from 0 to 2147483647"},
        {"-1:1:0,0,0,0\n", "line 1 \"-1:1:0,0,0,0\": its frame \"-1\" is not a whole number from 0 to 2147483647"},
        {"2147483648:1:0,0,0,0\n", "its frame \"2147483648\" is not a whole number from 0 to 2147483647"},
        {"5:g:0,0,0,0\n", "line 1 \"5:g:0,0,0,0\": its buttons \"g\" are not hex digits of 64 bits at most"},
        {"5::0,0,0,0\n", "line 1 \"5::0,0,0,0\": its buttons \"\" are not hex digits of 64 bits at most"},
        {"5:10000000000000000:0,0,0,0\n", "its buttons \"10000000000000000\" are not hex digits of 64 bits at most"},
        {"5:1:40000,0,0,0\n",
         "line 1 \"5:1:40000,0,0,0\": its lx \"40000\" is not a whole number from -32767 to 32767"},
        {"5:1:0,0,0,-32768\r\n", "line 1 \"5:1:0,0,0,-32768\": its ry \"-32768\" is not a whole number from -32767 to"},
        {"5:1:0, 0,0,0\n", "line 1 \"5:1:0, 0,0,0\": its ly \" 0\" is not a whole number from -32767 to 32767"},
        {"5:1:0,0,0,0\n7:2:0,0,0,0\n6:0:0,0,0,0\n",
         "line 3 \"6:0:0,0,0,0\": its frame 6 is not past frame 7 of line 2, the one before"},
        {"5:1:0,0,0,0\n#\n5:2:0,0,0,0\n", "line 3 \"5:2:0,0,0,0\": its frame 5 is not past frame 5 of line 1, the one"},
        {"\x7f"
         "ELF\x02\n",
         "line 1 \"\\x7fELF\\x02\": it is not frame:buttons:lx,ly,rx,ry"},
        {"1111111111111111111111111111111111111111111111111111111111111111111111111111111111\n",
         "line 1 \"111111111111111111111111111111111111111111111111111111111111... (82 bytes)\": it is not"},
    };
    char want[sizeof header + sizeof lines];
    size_t i;
    int s;
    snprintf(g_exedir, sizeof g_exedir, "%s/", dir);
    snprintf(g_exename, sizeof g_exename, "testgame");
    for (s = 0; s <= 9; s++) remove(slot_file(s));
    snprintf(want, sizeof want, "%s%s", header, lines);

    /* the record: slot 1's file, made at the first poll; a line a present the input changed, one a present at most,
       flushed as it goes */
    g_frames = 2;
    CHECK(!*refused("input_record", "1"), "input_record=1 refused: %s", g_fatal);
    CHECK(!got(slot_file(1)), "slot 1's file made before the first poll");
    poll(2, 0, 0, 0, 0, 0); /* neutral, as the record starts from: no line */
    CHECK(same(slot_file(1), header), "the header: \"%s\"", g_text);
    poll(3, 1, 0, 0, 0, 0);
    poll(3, 1, 0, 0, 0, 0);
    poll(3, 3, 0, 0, 0, 0); /* present 3 has its line: this change goes with present 4's */
    CHECK(!*refused("input_record", "1"), "a read of the same input_record refused: %s", g_fatal);
    poll(4, 3, 0, 0, 0, 0);
    poll(5, 3, -32767, 100, 0, 32767);
    poll(6, 3, -32767, 100, 0, 32767);
    poll(7, 0x8000, 0, 0, 0, 0);
    poll(7, 0, 0, 0, 0, 0); /* let go in present 7: written with present 8 */
    poll(8, 0, 0, 0, 0, 0);
    poll(9, 0, 0, 0, 0, 0);
    CHECK(same(slot_file(1), want), "the record wrote \"%s\"", g_text);
    CHECK(!*refused("input_record", "0"), "input_record=0 refused: %s", g_fatal);
    poll(10, 1, 0, 0, 0, 0);
    CHECK(same(slot_file(1), want), "a line after the record ended: \"%s\"", g_text);

    /* the replay: neutral before its first line, the last line at or before the present, the controls' input after
       its last line's present; # lines, empty ones and a \r before the \n */
    put(slot_file(3), "# a route\n\n10:1:0,0,0,0\n12:2:-32767,0,0,32767\r\n15:0:0,0,0,0\n");
    g_frames = 0;
    CHECK(!*refused("input_replay", "3"), "input_replay=3 refused: %s", g_fatal);
    poll(0, 0x80, 5, 5, 5, 5);
    CHECK(gave(0, 0, 0, 0, 0), "before the first line: " GAVE);
    poll(9, 0x80, 5, 5, 5, 5);
    CHECK(gave(0, 0, 0, 0, 0), "at present 9: " GAVE);
    poll(10, 0x80, 5, 5, 5, 5);
    CHECK(gave(1, 0, 0, 0, 0), "at present 10: " GAVE);
    /* a read of the settings naming the same slot does not read its file again, changed or not */
    put(slot_file(3), "10:40:0,0,0,0\n99:40:0,0,0,0\n");
    CHECK(!*refused("input_replay", "3"), "a read of the same input_replay refused: %s", g_fatal);
    poll(11, 0x80, 5, 5, 5, 5);
    CHECK(gave(1, 0, 0, 0, 0), "at present 11, after a read of the same input_replay: " GAVE);
    poll(12, 0x80, 5, 5, 5, 5);
    CHECK(gave(2, -32767, 0, 0, 32767), "at present 12: " GAVE);
    poll(15, 0x80, 5, 5, 5, 5);
    CHECK(gave(0, 0, 0, 0, 0), "at present 15, the last line's: " GAVE);
    poll(16, 0x80, 5, 5, 5, 5);
    CHECK(gave(0x80, 5, 5, 5, 5), "at present 16, past the last line: " GAVE);
    CHECK(!*refused("input_replay", "3"), "a read of the same input_replay refused: %s", g_fatal);
    poll(17, 0x40, 1, 2, 3, 4);
    CHECK(gave(0x40, 1, 2, 3, 4), "a read of the same input_replay played it again: " GAVE);
    /* presents gone by between two polls: the last line at or before the present */
    CHECK(!*refused("input_replay", "0"), "input_replay=0 refused: %s", g_fatal);
    put(slot_file(3), "20:1:0,0,0,0\n30:4:0,0,0,0\n40:0:0,0,0,0\n");
    CHECK(!*refused("input_replay", "3"), "input_replay=3 refused: %s", g_fatal);
    poll(35, 0x80, 5, 5, 5, 5);
    CHECK(gave(4, 0, 0, 0, 0), "at present 35: " GAVE);
    poll(50, 0x80, 5, 5, 5, 5);
    CHECK(gave(0x80, 5, 5, 5, 5), "at present 50: " GAVE);
    CHECK(!*refused("input_replay", "0"), "input_replay=0 refused: %s", g_fatal);

    /* slot 1 played back while slot 2 records: the same file, whatever the controls do */
    CHECK(!*refused("input_replay", "1"), "input_replay=1 refused: %s", g_fatal);
    g_frames = 0;
    CHECK(!*refused("input_record", "2"), "input_record=2 refused: %s", g_fatal);
    for (int f = 0; f <= 8; f++) poll(f, 0x80 | (u64)f, f, -f, 3, 4);
    CHECK(!*refused("input_record", "0"), "input_record=0 refused: %s", g_fatal);
    CHECK(!*refused("input_replay", "0"), "input_replay=0 refused: %s", g_fatal);
    CHECK(same(slot_file(2), want), "slot 2, the record of slot 1's replay: \"%s\"", g_text);

    /* recording a slot again replaces its file, and the log says what it held */
    g_log[0] = 0;
    CHECK(!*refused("input_record", "2"), "input_record=2 refused: %s", g_fatal);
    poll(20, 0, 0, 0, 0, 0);
    CHECK(
        strstr(g_log, "input_record: slot 2 replaces its old recording of 9 frames (5 lines, the last at frame 8), ") &&
            strstr(g_log, slot_file(2)),
        "the log of slot 2 replaced: \"%s\"", g_log);
    CHECK(same(slot_file(2), header), "slot 2 after it was replaced: \"%s\"", g_text);
    CHECK(!*refused("input_record", "0"), "input_record=0 refused: %s", g_fatal);
    g_log[0] = 0;
    CHECK(!*refused("input_record", "2"), "input_record=2 refused: %s", g_fatal);
    poll(21, 0, 0, 0, 0, 0);
    CHECK(strstr(g_log, "input_record: slot 2 replaces its old recording, which has no lines, "),
          "the log of slot 2 replaced with no lines: \"%s\"", g_log);
    CHECK(!*refused("input_record", "0"), "input_record=0 refused: %s", g_fatal);
    put(slot_file(2), "not one\n");
    g_log[0] = 0;
    CHECK(!*refused("input_record", "2"), "input_record=2 refused: %s", g_fatal);
    poll(22, 0, 0, 0, 0, 0);
    CHECK(strstr(g_log, ", which is no recording: line 1 \"not one\": it is not frame:buttons:lx,ly,rx,ry"),
          "the log of slot 2 replaced over no recording: \"%s\"", g_log);
    CHECK(same(slot_file(2), header), "slot 2 after it was replaced: \"%s\"", g_text);
    CHECK(!*refused("input_record", "0"), "input_record=0 refused: %s", g_fatal);

    /* a slot outside 0 to 9 ends the run with the value given: ini_read would have made it slot 9 or 0 */
    CHECK(!strcmp(refused("input_record", "12"), "input_record=12 is not a slot: 1 to 9, or 0 for none"),
          "input_record=12: \"%s\"", g_fatal);
    poll(30, 1, 0, 0, 0, 0);
    CHECK(!got(slot_file(9)), "input_record=12 recorded to slot 9");
    CHECK(!strcmp(refused("input_replay", "-1"), "input_replay=-1 is not a slot: 1 to 9, or 0 for none"),
          "input_replay=-1: \"%s\"", g_fatal);
    CHECK(!strcmp(refused("input_record", "2.5"), "input_record=2.5 is not a slot: 1 to 9, or 0 for none"),
          "input_record=2.5: \"%s\"", g_fatal);
    poll(31, 1, 0, 0, 0, 0);
    CHECK(same(slot_file(2), header), "input_record=2.5 recorded to slot 2: \"%s\"", g_text);

    /* one slot both recorded and played, in either order: refused before its file is touched */
    put(slot_file(6), "3:1:0,0,0,0\n");
    CHECK(!*refused("input_replay", "6"), "input_replay=6 refused: %s", g_fatal);
    CHECK(strstr(refused("input_record", "6"), "input_record and input_replay are both slot 6: recording it would "
                                               "replace the recording it plays; record to another slot"),
          "input_record=6 while slot 6 plays: \"%s\"", g_fatal);
    poll(1, 0x80, 5, 5, 5, 5);
    CHECK(same(slot_file(6), "3:1:0,0,0,0\n"), "slot 6 changed while it played: \"%s\"", g_text);
    CHECK(!*refused("input_replay", "0"), "input_replay=0 refused: %s", g_fatal);
    CHECK(!*refused("input_record", "6"), "input_record=6 refused: %s", g_fatal);
    CHECK(strstr(refused("input_replay", "6"), "input_record and input_replay are both slot 6"),
          "input_replay=6 while slot 6 is to be recorded: \"%s\"", g_fatal);
    CHECK(same(slot_file(6), "3:1:0,0,0,0\n"), "slot 6 changed before a poll: \"%s\"", g_text);
    CHECK(!*refused("input_record", "0"), "input_record=0 refused: %s", g_fatal);

    /* each line a replay refuses, by its slot, its number and its text; the controls' input reaches player 1 after */
    for (i = 0; i < sizeof bad / sizeof *bad; i++) {
        put(slot_file(4), bad[i].text);
        CHECK(strstr(refused("input_replay", "4"), bad[i].says), "replay %zu: \"%s\", not \"%s\"", i, g_fatal,
              bad[i].says);
        CHECK(!*refused("input_replay", "0"), "input_replay=0 refused: %s", g_fatal);
        poll(60, 0x80, 5, 5, 5, 5);
        CHECK(gave(0x80, 5, 5, 5, 5), "replay %zu was refused, then the game took %llx:%d,%d,%d,%d", i,
              (unsigned long long)g_b, g_st[0], g_st[1], g_st[2], g_st[3]);
    }
    CHECK(!strcmp(refused("input_replay", "5"), "input_replay: slot 5 has no recording: input_record=5 makes one"),
          "input_replay of an empty slot: \"%s\"", g_fatal);
    CHECK(!*refused("input_replay", "0"), "input_replay=0 refused: %s", g_fatal);
    for (s = 0; s <= 9; s++) remove(slot_file(s));
}

int main(int argc, char** argv) {
    g_now = 1000000000ull;
    unattended_start();
    unattended_ini("script", "1000:1,5f:2,20f:400");
    CHECK(at(0, 0) == 0, "nothing at the start");
    CHECK(at(999, 4) == 0, "nothing before its time or present");
    CHECK(at(1000, 4) == 1, "A at 1000 ms: %llx", (unsigned long long)at(1000, 4));
    /* present 5 comes at 2000 ms: B is held from then, not from when the script was read */
    CHECK(at(2000, 5) == 2, "B from present 5: %llx", (unsigned long long)at(2000, 5));
    unattended_ini("script", "1000:1,5f:2,20f:400");
    CHECK(at(2299, 9) == 2, "B held 300 ms: %llx", (unsigned long long)at(2299, 9));
    CHECK(at(2300, 9) == 0, "B released after 300 ms: %llx", (unsigned long long)at(2300, 9));
    /* a present skipped over (frames go by two at a time) still presses */
    CHECK(at(5000, 21) == 0x400, "Plus once past present 20: %llx", (unsigned long long)at(5000, 21));
    CHECK(at(9000, 400) == 0, "each press once: %llx", (unsigned long long)at(9000, 400));
    CHECK(unattended_ms() == 9000, "the clock --script counts from: %llu", (unsigned long long)unattended_ms());
    unattended_ini("script", "1000:1,5f:2,20f:400");
    CHECK(at(9001, 401) == 0, "reloading unchanged script does not replay completed presses");
    unattended_ini("script", "402f:1");
    CHECK(at(9010, 402) == 1, "changed script starts its new press");
    CHECK(at(9310, 403) == 0, "new press ends normally");

    /* the help's example of a present is one the parser takes as a present */
    {
        const IniOpt* o = ini_find(g_unattended_ini, "script");
        CHECK(o && o->help && strstr(o->help, "(300f)"), "script's help gives no 300f: %s", o && o->help ? o->help : "");
        unattended_ini("script", "300f:1");
        CHECK(at(9020, 299) == 0 && at(9030, 300) == 1, "300f is no present's number");
    }
    /* a malformed item ends the script there, the items before it kept */
    unattended_ini("script", "500f:8,oops,600f:10");
    CHECK(at(9500, 500) == 8, "the item before a malformed one still presses");
    CHECK(at(9900, 600) == 0, "an item after a malformed one pressed");
    CHECK(unattended_known("shot_every") && !unattended_known("touch"), "unattended_known");

    /* the limits, and numbered shots by bucket even when frames go by two */
    CHECK(!unattended_active(), "active without seconds or frames");
    unattended_ini("frames", "700");
    unattended_ini("shot", "out/run");
    unattended_ini("shot_every", "10");
    CHECK(unattended_active() && !unattended_done(), "frames=700 not active, or done too early");
    {
        char path[64];
        g_frames = 605;
        CHECK(unattended_periodic_shot(path, sizeof path) && !strcmp(path, "out/run_0000"), "first numbered shot");
        g_frames = 609;
        CHECK(!unattended_periodic_shot(path, sizeof path), "the same bucket shot twice");
        g_frames = 611; /* 610 went by between two looks */
        CHECK(unattended_periodic_shot(path, sizeof path) && !strcmp(path, "out/run_0001"), "a bucket skipped over");
        g_frames = 621;
        CHECK(unattended_periodic_shot(path, sizeof path) && !strcmp(path, "out/run_0002"), "third numbered shot");
    }
    g_frames = 700;
    CHECK(unattended_done(), "frames=700 not done at 700");
    unattended_ini("shot", "");
    CHECK(!unattended_shot(), "an empty shot does not clear it");

    /* the stick keys, and the clamp into the circle */
    {
        i32 st[4], x = 32767, y = 32767;
        input_key_sticks(1ull << 40 | 1ull << 41 | 1ull << 46 | 1ull << 44, st);
        CHECK(st[0] == -32767 && st[1] == 32767 && st[2] == 0 && st[3] == 0, "stick keys %d %d %d %d", st[0], st[1],
              st[2], st[3]);
        input_key_sticks(1ull << 47, st);
        CHECK(st[2] == 0 && st[3] == -32767, "right stick down %d %d", st[2], st[3]);
        input_stick_clamp(&x, &y);
        CHECK(x == y && x > 23000 && x < 23171 && (double)x * x + (double)y * y <= 32767.0 * 32767.0,
              "diagonal clamp %d %d", x, y);
        x = -40000;
        y = 0;
        input_stick_clamp(&x, &y);
        CHECK(x == -32767 && y == 0, "axis clamp %d %d", x, y);
    }

    if (argc < 2) {
        printf("FAIL: no folder for the game's input_record and input_replay slots (ctest gives its build folder)\n");
        return 1;
    }
    tas(argv[1]);

    /* last, as it starts the clock over: a press the backend looked at before the game started (during main.c's
       loading screen) is pressed again once unattended_start starts the game's clock, not used up before it */
    g_now = 1000000000ull + 20000ull * 1000000ull;
    unattended_start();
    unattended_ini("script", "703f:4");
    CHECK(at(20000, 703) == 4 && at(20400, 703) == 0, "703f pressed and ended before the start");
    g_now = 1000000000ull + 30000ull * 1000000ull;
    unattended_start();
    CHECK(at(30000, 703) == 4, "703f pressed again after unattended_start: %llx", (unsigned long long)at(30000, 703));
    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
