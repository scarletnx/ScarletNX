/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Unattended runs, for the windowed and the headless backend alike: the run's limit, screenshots, scripted presses,
   and player 1's input recorded to the game's slots and played back from them */
#include "nx/runtime/platform/unattended.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* one press of the script: at a time in ms, or at a present ("Nf") */
typedef struct {
    u64 mask;
    u64 held_since; /* plat_ns() when a present press began, 0 before */
    u32 at;
    u8 at_present;
    u8 done;
} ScriptPress;

/* The script: its presses and, after them in the same allocation, its text (to tell a reload of the same script
   from a new one). press is the allocation, NULL without a script */
typedef struct {
    ScriptPress* press;
    char* text;
    size_t n;
} Script;

/* from this frame (presents since the game's start) on, these buttons (HidNpadButton bits) and sticks */
typedef struct {
    u64 buttons;
    i32 st[4]; /* lx, ly, rx, ry */
    int frame;
} TasLine;

/* The record's file and the replay's lines, the main thread's: the backend's poll and the settings' reads use them.
   The settings are read again whenever the .ini changes, and a read that gives the slot a record or a replay has
   already goes on with it: their slots are kept for that */
typedef struct {
    FILE* record;    /* record_slot's file, made at the first poll after the slot was set; NULL before and without */
    TasLine written; /* the record's last line: frame -1 and a neutral pad before its first */
    TasLine* replay; /* the replay's lines, frames rising, one allocation; NULL without one or past its end */
    size_t lines;    /* how many */
    size_t next;     /* the first line whose frame no poll reached yet */
    int record_slot; /* 1 to 9, 0 none */
    int replay_slot;
} Tas;

/* The file's state. The settings are g_settings' (settings.txt's g_unattended_ini): seconds (0: no time limit), frames,
   screenshot, screenshot_every, press_length, script, input_record, input_replay */
static struct {
    Script script;
    u64 t0;          /* unattended_start's plat_ns() */
    int shot_bucket; /* frames / shot_every at the last periodic shot */
    int shot_count;
    Tas tas;
} g_unattended;

static int script_parse_press(const char** s, ScriptPress* p) {
    const char* c = *s;
    char* end;
    unsigned long at;
    unsigned long long mask;
    if (*c < '0' || *c > '9') return 0;
    at = strtoul(c, &end, 10);
    c = end;
    memset(p, 0, sizeof *p);
    if (*c == 'f') {
        p->at_present = 1;
        c++;
    }
    if (*c++ != ':' || at > UINT_MAX) return 0;
    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F'))) return 0;
    mask = strtoull(c, &end, 16);
    c = end;
    if (*c && *c != ',') return 0;
    p->at = (u32)at;
    p->mask = (u64)mask;
    *s = *c ? c + 1 : c;
    return 1;
}

void unattended_script_set(const IniOpt* o, const char* v) {
    size_t len = strlen(v), max = 1, n;
    const char* c;
    ScriptPress* press;
    (void)o;
    if (g_unattended.script.press && !strcmp(g_unattended.script.text, v))
        return; /* a reload: keep the progress, presses aren't replayed */
    if (!*v) {
        free(g_unattended.script.press);
        memset(&g_unattended.script, 0, sizeof g_unattended.script);
        return;
    }
    for (c = v; *c; c++) max += *c == ',';
    if (max > (SIZE_MAX - len - 1) / sizeof *press) {
        WARN("unattended: the script is too long, the current one stays\n");
        return;
    }
    press = malloc(max * sizeof *press + len + 1);
    if (!press) {
        WARN("unattended: out of memory for the script, the current one stays\n");
        return;
    }
    for (n = 0, c = v; *c && n < max; n++)
        if (!script_parse_press(&c, &press[n])) {
            WARN("unattended: the script stops at \"%s\", which is not ms:hex or Nf:hex\n", c);
            break;
        }
    free(g_unattended.script.press);
    g_unattended.script.press = press;
    g_unattended.script.text = (char*)(press + max);
    memcpy(g_unattended.script.text, v, len + 1);
    g_unattended.script.n = n;
}

int unattended_known(const char* k) { return ini_find(g_unattended_ini, k) != NULL; }
int unattended_ini(const char* k, const char* v) {
    const IniOpt* o = ini_find(g_unattended_ini, k);
    if (!o) return 0;
    ini_read(o, v);
    return 1;
}

/* the presses start over too: a look at them before (the backend's, during main.c's prepare_wait) began a present
   press, which would end before the game's presents reach it */
void unattended_start(void) {
    g_unattended.t0 = plat_ns();
    for (size_t i = 0; i < g_unattended.script.n; i++) {
        g_unattended.script.press[i].held_since = 0;
        g_unattended.script.press[i].done = 0;
    }
}
u64 unattended_ms(void) { return (plat_ns() - g_unattended.t0) / 1000000ull; }
int unattended_active(void) { return g_settings.seconds > 0 || g_settings.frames >= 0; }

static int frames_now(void) { return __atomic_load_n(&g_frames, __ATOMIC_RELAXED); }

/* ---- input_record and input_replay: player 1's input in the game's slots, a line "frame:buttons:lx,ly,rx,ry" a
   present it changed */

#define SNX_TAS_EXT ".snxtas"

/* slot's file, <g_exedir>tas/<g_exename>.<slot>.snxtas: the game's folder and name, as state_path makes a save
   state's */
static void tas_path(char* out, size_t size, int slot) {
    snprintf(out, size, "%stas/%s.%d" SNX_TAS_EXT, g_exedir, g_exename, slot);
}

/* the record's lines are in its file as they are written, so a run that ends in a crash keeps them */
static void tas_flush(int frame) {
    Tas* t = &g_unattended.tas;
    char path[sizeof g_exedir + sizeof g_exename + 64];
    int e;
    if (!ferror(t->record) && !fflush(t->record)) return;
    e = errno;
    tas_path(path, sizeof path, t->record_slot);
    LOG("input_record: writing %s failed at frame %d\n", path, frame);
    fatal("input_record: writing slot %d's file failed at frame %d: %s", t->record_slot, frame, strerror(e));
}

/* the whole number of s[0, len), decimal digits after an optional -: 1 and *v when from lo to hi, else 0 */
static int tas_int(const char* s, size_t len, long long lo, long long hi, long long* v) {
    size_t i = len && s[0] == '-';
    long long x = 0;
    if (i == len || len - i > 10) return 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        x = x * 10 + (s[i] - '0');
    }
    *v = s[0] == '-' ? -x : x;
    return *v >= lo && *v <= hi;
}

/* the hex digits of s[0, len), 64 bits at most: 1 and *v, else 0 */
static int tas_hex(const char* s, size_t len, u64* v) {
    size_t i, digits = 0;
    u64 x = 0;
    if (!len) return 0;
    for (i = 0; i < len; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        if (d < 0) return 0;
        digits += x || d; /* leading zeros take no bits */
        if (digits > 16) return 0;
        x = x << 4 | (u64)d;
    }
    *v = x;
    return 1;
}

/* A line of a recording, s[0, len): 1 and *l, else 0 and why in why */
static int tas_parse(const char* s, size_t len, TasLine* l, char* why, size_t n) {
    static const char* const axis[4] = {"lx", "ly", "rx", "ry"};
    size_t at[7] = {0}, f = 0, i;
    long long v;
    /* the fields' starts: a : after the frame and the buttons, a , between the sticks */
    for (i = 0; i < len; i++) {
        if (s[i] != ':' && s[i] != ',') continue;
        if (f == 5 || s[i] != (f < 2 ? ':' : ',')) break;
        at[++f] = i + 1;
    }
    if (f < 5 || i < len) {
        snprintf(why, n, "it is not frame:buttons:lx,ly,rx,ry");
        return 0;
    }
    at[6] = len + 1;
    for (f = 0; f < 6; f++) {
        const char* t = s + at[f];
        size_t tl = at[f + 1] - 1 - at[f];
        int shown = tl > 24 ? 24 : (int)tl;
        if (f == 0 && !tas_int(t, tl, 0, INT_MAX, &v))
            snprintf(why, n, "its frame \"%.*s\" is not a whole number from 0 to %d", shown, t, INT_MAX);
        else if (f == 1 && !tas_hex(t, tl, &l->buttons))
            snprintf(why, n, "its buttons \"%.*s\" are not hex digits of 64 bits at most", shown, t);
        else if (f > 1 && !tas_int(t, tl, -32767, 32767, &v))
            snprintf(why, n, "its %s \"%.*s\" is not a whole number from -32767 to 32767", axis[f - 2], shown, t);
        else {
            if (f == 0) l->frame = (int)v;
            if (f > 1) l->st[f - 2] = (i32)v;
            continue;
        }
        return 0;
    }
    return 1;
}

/* up to 60 bytes of a line for a message: a byte outside printable ASCII as \xNN, and the line's size after "..." when
   it is longer */
static void tas_excerpt(const char* s, size_t len, char* out, size_t n) {
    size_t i, o = 0;
    for (i = 0; i < len && i < 60 && o + 5 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c < 0x7f)
            out[o++] = (char)c;
        else
            o += (size_t)snprintf(out + o, n - o, "\\x%02x", c);
    }
    out[o] = 0;
    if (i < len) snprintf(out + o, n - o, "... (%zu bytes)", len);
}

/* The lines of a recording's text, frames rising, past # lines and empty ones (a \r before a \n ends a line too), into
   out unless it is NULL (room for a line a \n, and one more): 1 with how many in *count and the last one's frame in
   *last (-1 with none), else 0 with the line refused, its number and why in msg */
static int tas_lines(const char* text, size_t size, TasLine* out, size_t* count, int* last, char* msg, size_t n) {
    size_t at, line = 0, before = 0, len, text_len, k = 0;
    TasLine l = {0};
    int prev = -1;
    char why[160], shown[300];
    for (at = 0; at < size; at += len + 1) {
        const char* s = text + at;
        const char* nl = memchr(s, '\n', size - at);
        len = nl ? (size_t)(nl - s) : size - at;
        line++;
        if (!len || s[0] == '#') continue;
        text_len = len - (s[len - 1] == '\r');
        if (!text_len) continue;
        if (tas_parse(s, text_len, &l, why, sizeof why)) {
            if (l.frame > prev) {
                if (out) out[k] = l;
                k++;
                prev = l.frame;
                before = line;
                continue;
            }
            snprintf(why, sizeof why, "its frame %d is not past frame %d of line %zu, the one before", l.frame, prev,
                     before);
        }
        tas_excerpt(s, text_len, shown, sizeof shown);
        snprintf(msg, n, "line %zu \"%s\": %s", line, shown, why);
        return 0;
    }
    *count = k;
    *last = prev;
    return 1;
}

/* The slot key=v names. ini_read took v as a whole number from 0 to 9 and made one outside that the nearest end, which
   is another slot, so it ends the run here; v not a number never comes here, as ini_read keeps the slot it had */
static int tas_slot(const char* key, const char* v) {
    long long slot;
    if (!tas_int(v, strlen(v), 0, 9, &slot)) fatal("%s=%s is not a slot: 1 to 9, or 0 for none", key, v);
    return (int)slot;
}

/* a slot recorded while it plays would lose the recording it plays */
static void tas_apart(int record, int replay) {
    if (record && record == replay)
        fatal("input_record and input_replay are both slot %d: recording it would replace the recording it plays; "
              "record to another slot",
              record);
}

/* record_slot's file, made at the first poll after the slot was set (the settings' read has set input_replay too by
   then): what the slot held is said, then replaced */
static void tas_record_open(int frame) {
    Tas* t = &g_unattended.tas;
    char path[sizeof g_exedir + sizeof g_exename + 64], folder[sizeof g_exedir + 8], msg[600];
    unsigned char* old;
    size_t size, count = 0;
    int last = -1, e;
    tas_path(path, sizeof path, t->record_slot);
    old = snx_read_file(path, &size);
    if (old) {
        if (!tas_lines((const char*)old, size, NULL, &count, &last, msg, sizeof msg))
            LOG("input_record: slot %d replaces %s, which is no recording: %s\n", t->record_slot, path, msg);
        else if (!count)
            LOG("input_record: slot %d replaces its old recording, which has no lines, %s\n", t->record_slot, path);
        else
            LOG("input_record: slot %d replaces its old recording of %d frames (%zu lines, the last at frame %d), %s\n",
                t->record_slot, last + 1, count, last, path);
        free(old);
    }
    snprintf(folder, sizeof folder, "%stas", g_exedir);
    plat_mkdir(folder);
    LOG("input_record: player 1's input goes to slot %d, %s, from frame %d\n", t->record_slot, path, frame);
    t->record = snx_fopen(path, "wb"); /* binary: Windows' text mode would end each line with a CR */
    if (!t->record) {
        e = errno;
        fatal("input_record: slot %d's file could not be made: %s", t->record_slot, strerror(e));
    }
    t->written = (TasLine){.frame = -1};
    fprintf(t->record,
            "# input_record of %s\n"
            "# frame:buttons:lx,ly,rx,ry each present player 1's input changed: frame the presents since the game's "
            "start, buttons in hex\n",
            snx_build_info());
    tas_flush(frame);
}

void unattended_record_set(const IniOpt* o, const char* v) {
    Tas* t = &g_unattended.tas;
    int slot = tas_slot(o->key, v), frame = frames_now(), e;
    if (slot == t->record_slot) return; /* the slot it records already */
    tas_apart(slot, t->replay_slot);
    if (t->record) {
        LOG("input_record: slot %d ends at frame %d\n", t->record_slot, frame);
        e = fclose(t->record) ? errno : 0;
        t->record = NULL;
        if (e) fatal("input_record: closing slot %d's file failed at frame %d: %s", t->record_slot, frame, strerror(e));
    }
    t->record_slot = slot; /* its file is made at the next poll, tas_record_open */
}

void unattended_replay_set(const IniOpt* o, const char* v) {
    Tas* t = &g_unattended.tas;
    char path[sizeof g_exedir + sizeof g_exename + 64], msg[600];
    unsigned char* text;
    size_t size, max = 1, i;
    int slot = tas_slot(o->key, v), frame = frames_now(), e, last = -1, ok;
    if (slot == t->replay_slot) return; /* the slot it plays already, or played to its end */
    tas_apart(t->record_slot, slot);
    if (t->replay)
        LOG("input_replay: slot %d stops at frame %d, its last line's frame %d\n", t->replay_slot, frame,
            t->replay[t->lines - 1].frame);
    free(t->replay);
    t->replay = NULL;
    t->lines = t->next = 0;
    t->replay_slot = slot;
    if (!slot) return;
    tas_path(path, sizeof path, slot);
    LOG("input_replay: player 1's input from slot %d, %s\n", slot, path);
    text = snx_read_file(path, &size);
    if (!text) {
        e = errno;
        if (e == ENOENT) fatal("input_replay: slot %d has no recording: input_record=%d makes one", slot, slot);
        fatal("input_replay: slot %d's file could not be read: %s", slot, strerror(e));
    }
    for (i = 0; i < size; i++) max += text[i] == '\n';
    if (max > SIZE_MAX / sizeof *t->replay) {
        free(text);
        fatal("input_replay: slot %d's %zu lines are more than this machine can hold", slot, max);
    }
    t->replay = snx_alloc(max * sizeof *t->replay);
    ok = tas_lines((const char*)text, size, t->replay, &t->lines, &last, msg, sizeof msg);
    free(text);
    if (!ok || !t->lines) {
        free(t->replay);
        t->replay = NULL;
    }
    if (!ok) fatal("input_replay: slot %d, %s", slot, msg);
    if (!t->lines)
        LOG("input_replay: slot %d has no lines, so player 1's input comes from the controls at frame %d already\n",
            slot, frame);
    else
        LOG("input_replay: %zu lines, frames %d to %d: player 1's input is neutral before frame %d and comes from "
            "the controls again at frame %d\n",
            t->lines, t->replay[0].frame, last, t->replay[0].frame, last + 1);
}

void unattended_pad(u64* buttons, i32 st[4]) {
    Tas* t = &g_unattended.tas;
    int frame = frames_now();
    if (t->replay && frame > t->replay[t->lines - 1].frame) {
        LOG("input_replay: slot %d ends at frame %d, past its last line's frame %d: player 1's input comes from the "
            "controls again\n",
            t->replay_slot, frame, t->replay[t->lines - 1].frame);
        free(t->replay);
        t->replay = NULL;
        t->lines = t->next = 0;
    }
    if (t->replay) {
        while (t->next < t->lines && t->replay[t->next].frame <= frame) t->next++;
        if (t->next) {
            *buttons = t->replay[t->next - 1].buttons;
            memcpy(st, t->replay[t->next - 1].st, sizeof t->replay->st);
        } else { /* before its first line */
            *buttons = 0;
            memset(st, 0, sizeof t->replay->st);
        }
    }
    if (t->record_slot && !t->record) tas_record_open(frame);
    /* a line a present at most: a change in the present of the last line is written with the next present's */
    if (t->record && frame > t->written.frame &&
        (*buttons != t->written.buttons || memcmp(st, t->written.st, sizeof t->written.st))) {
        t->written.frame = frame;
        t->written.buttons = *buttons;
        memcpy(t->written.st, st, sizeof t->written.st);
        fprintf(t->record, "%d:%llx:%d,%d,%d,%d\n", frame, (unsigned long long)*buttons, (int)st[0], (int)st[1],
                (int)st[2], (int)st[3]);
        tas_flush(frame);
    }
}

u64 unattended_buttons(void) {
    u64 now = plat_ns(), held = 0, len_ns = (u64)g_settings.press_length * 1000000u, ms;
    int frames = frames_now();
    size_t i;
    if (!g_unattended.t0) g_unattended.t0 = now;
    ms = (now - g_unattended.t0) / 1000000u;
    for (i = 0; i < g_unattended.script.n; i++) {
        ScriptPress* p = &g_unattended.script.press[i];
        if (!p->at_present) {
            if (ms >= p->at && ms - p->at < (u64)g_settings.press_length) held |= p->mask;
            continue;
        }
        if (p->done) continue;
        /* from the first look at or after present N, even when the presents went past it between two looks: the
           game advances a step per present, so a time in ms could come too early on a slow machine */
        if (!p->held_since && frames >= 0 && (u32)frames >= p->at) p->held_since = now;
        if (!p->held_since) continue;
        if (now - p->held_since < len_ns)
            held |= p->mask;
        else
            p->done = 1;
    }
    return held;
}

const char* unattended_shot(void) { return g_settings.screenshot[0] ? g_settings.screenshot : NULL; }

int unattended_periodic_shot(char* path, size_t n) {
    int bucket;
    if (!g_settings.screenshot[0] || g_settings.screenshot_every <= 0) return 0;
    /* by bucket, not frames % shot_every: frames may move by two between looks */
    bucket = frames_now() / g_settings.screenshot_every;
    if (bucket == g_unattended.shot_bucket) return 0;
    g_unattended.shot_bucket = bucket;
    snprintf(path, n, "%s_%04d", g_settings.screenshot, g_unattended.shot_count++);
    return 1;
}

int unattended_done(void) {
    if (g_settings.frames >= 0 && frames_now() >= g_settings.frames) return 1;
    return g_settings.seconds > 0 && g_unattended.t0 &&
           plat_ns() - g_unattended.t0 >= (u64)g_settings.seconds * 1000000000u;
}

void unattended_exit(u64 hash, int w, int h) {
    if (w > 0 && h > 0)
        fprintf(stderr, "\nframes=%d hash=%016llx %dx%d\n", frames_now(), (unsigned long long)hash, w, h);
    else
        fprintf(stderr, "\nframes=%d hash=%016llx\n", frames_now(), (unsigned long long)hash);
    ini_poll(1); /* the settings changed during the run, as a quit saves them */
    snx_save_before_exit();
    /* The same close as process_exit: it ends the frontend's open zone and lets the capture write. */
    snx_tracy_shutdown();
    plat_exit_now(0);
}

void input_key_sticks(u64 k, i32 st[4]) {
    int s;
    for (s = 0; s < 2; s++) {
        u64 d = k >> (40 + 4 * s); /* left, up, right, down */
        st[2 * s] = ((i32)((d >> 2) & 1) - (i32)(d & 1)) * 32767;
        st[2 * s + 1] = ((i32)((d >> 1) & 1) - (i32)((d >> 3) & 1)) * 32767;
    }
}

void input_stick_clamp(i32* x, i32* y) {
    double r;
    if (*x < -32767) *x = -32767;
    if (*x > 32767) *x = 32767;
    if (*y < -32767) *y = -32767;
    if (*y > 32767) *y = 32767;
    r = sqrt((double)*x * *x + (double)*y * *y);
    if (r > 32767.0) {
        *x = (i32)(*x * (32767.0 / r));
        *y = (i32)(*y * (32767.0 / r));
    }
}
