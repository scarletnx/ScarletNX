/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The log summary (logscan.h). The lines are matched by hand, prefix and substring, in the formats the runtime
   prints them: the header (main.c), the failure markers (crash.c, svc.c, rtapi.c, arm.c, plat_*.c), the register
   dump and backtrace (hle/kernel/debug.c), ipc.c's service command warnings, and what the runtime missed (gap_line) */
#include "common/utilities/logscan.h"
#include "common/utilities/ascii.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ text */
/* a piece of the log: never stored, the log outlives the scan's use of it */
typedef struct {
    const char* p;
    size_t n;
} Span;

static Span span(const char* p, size_t n) {
    Span s;
    s.p = p;
    s.n = n;
    return s;
}
static Span cstr(const char* p) { return span(p, strlen(p)); }
static Span skip(Span s, size_t k) { return k >= s.n ? span(s.p + s.n, 0) : span(s.p + k, s.n - k); }
static int starts(Span s, const char* lit) {
    size_t n = strlen(lit);
    return s.n >= n && !memcmp(s.p, lit, n);
}
static int ends(Span s, const char* lit) {
    size_t n = strlen(lit);
    return s.n >= n && !memcmp(s.p + s.n - n, lit, n);
}
/* where lit is in s, or -1: memchr finds its first byte's candidates */
static long find(Span s, const char* lit) {
    size_t n = strlen(lit);
    const char *p = s.p, *end = s.p + s.n;
    if (!n) return 0;
    while (n <= (size_t)(end - p) && (p = (const char*)memchr(p, lit[0], (size_t)(end - p) - n + 1)) != NULL) {
        if (!memcmp(p, lit, n)) return (long)(p - s.p);
        p++;
    }
    return -1;
}
/* lit, lower case, at s + i in any case. c | 0x20 is exact for the letters, and keeps ',' and ' ' (only the controls
   0x0c and 0x00 also land on them) */
static int lower_at(Span s, size_t i, const char* lit) {
    for (size_t j = 0; lit[j]; j++)
        if (i + j >= s.n || (s.p[i + j] | 0x20) != lit[j]) return 0;
    return 1;
}
/* a warning's words, in any case, in one pass over the line: each starts with a 'u', but for "is unknown" (a "u" with
   "is " before it) and "not implemented" */
static int warning_word(Span s) {
    for (size_t i = 0; i < s.n; i++) {
        int c = s.p[i] | 0x20;
        if (c == 'u') {
            if (lower_at(s, i, "unsupported") || lower_at(s, i, "unimplemented") ||
                lower_at(s, i, "unknown, invalidenumvalue") ||
                (i >= 3 && lower_at(s, i - 3, "is ") && lower_at(s, i, "unknown")))
                return 1;
        } else if (c == 'n' && lower_at(s, i, "not implemented"))
            return 1;
    }
    return 0;
}
/* lower case only, as the runtime prints addresses and hashes: an upper-case word (a format such as BC7) is a word */
static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
static size_t hex_run(Span s) {
    size_t i = 0;
    while (i < s.n && is_hex(s.p[i])) i++;
    return i;
}
static size_t dec_run(Span s) {
    size_t i = 0;
    while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') i++;
    return i;
}
static size_t space_run(Span s) {
    size_t i = 0;
    while (i < s.n && snx_is_blank(s.p[i])) i++;
    return i;
}
static Span trim(Span s) {
    while (s.n && (snx_is_blank(s.p[0]) || s.p[0] == '\r')) s = skip(s, 1);
    while (s.n && (snx_is_blank(s.p[s.n - 1]) || s.p[s.n - 1] == '\r')) s.n--;
    return s;
}
static int blank(Span s) { return trim(s).n == 0; }

/* "[   2.25] " before a line: its length, 0 without one, and the time in hundredths through *cs. The runtime prints it
   with %f, so a library that set the process's locale makes it "[   2,25] " */
static size_t stamp(Span s, long* cs) {
    size_t i = 1;
    long whole = 0, frac = 0;
    if (!s.n || s.p[0] != '[') return 0;
    while (i < s.n && s.p[i] == ' ') i++;
    size_t d = dec_run(skip(s, i));
    if (!d || d > 9 || i + d >= s.n || (s.p[i + d] != '.' && s.p[i + d] != ',')) return 0;
    for (long k = 0; k < (long)d; k++) whole = whole * 10 + (s.p[i + k] - '0');
    i += d + 1;
    d = dec_run(skip(s, i));
    if (!d || i + d >= s.n || s.p[i + d] != ']') return 0;
    for (long k = 0; k < 2; k++) frac = frac * 10 + (k < (long)d ? s.p[i + k] - '0' : 0);
    i += d + 1;
    if (i < s.n && s.p[i] == ' ') i++;
    if (cs) *cs = whole * 100 + frac;
    return i;
}

/* ============================================================ output */
typedef struct {
    char* p;
    size_t n, cap;
    int failed;
} Buf;

static void put_n(Buf* b, const char* p, size_t n) {
    char* grown;
    size_t cap;
    if (b->failed) return;
    if (n > SIZE_MAX / 2 - b->n - 1) {
        b->failed = 1;
        return;
    }
    if (b->n + n + 1 > b->cap) {
        cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        grown = (char*)realloc(b->p, cap);
        if (!grown) {
            b->failed = 1;
            return;
        }
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void put(Buf* b, const char* p) { put_n(b, p, strlen(p)); }
static void put_num(Buf* b, unsigned long v) {
    char num[24];
    sprintf(num, "%lu", v);
    put(b, num);
}
/* the buffer's text, handed over, or NULL when it ran out of memory */
static char* take(Buf* b) {
    if (b->failed) {
        free(b->p);
        return NULL;
    }
    if (!b->p) put_n(b, "", 0);
    return b->p;
}

/* ============================================================ the pool */
/* the length of the valid UTF-8 sequence at p (1 to 4), or 0 for an invalid byte */
static size_t utf8_len(const unsigned char* p, size_t n) {
    unsigned c = p[0];
    if (c < 0x80) return 1;
    size_t len = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
    if (!len || len > n) return 0;
    for (size_t i = 1; i < len; i++)
        if ((p[i] & 0xc0) != 0x80) return 0;
    if (len == 3 && ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] >= 0xa0))) return 0;
    if (len == 4 && ((c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] >= 0x90))) return 0;
    return len;
}

/* How long the home folder's part is at s: "/home/<user>", "/var/home/<user>" (Silverblue's), "/Users/<user>",
   "/media/<user>" and "/run/media/<user>" (the drives a desktop mounts for its user, where games often are),
   "C:\Users\<user>" (either slash, any case), Android's "/data/user/<n>/<package>"; 0 elsewhere. The player's account
   name stays out of a public report */
static size_t home_at(Span s) {
    static const char* const homes[] = {"/home/", "/var/home/", "/Users/", "/run/media/", "/media/"};
    size_t i = 0, k;
    if (!s.n || (s.p[0] != '/' && !snx_is_alpha(s.p[0]))) return 0;
    for (k = 0; k < sizeof homes / sizeof homes[0] && !i; k++)
        if (starts(s, homes[k])) i = strlen(homes[k]);
    if (!i && s.n > 9 && snx_is_alpha(s.p[0]) && s.p[1] == ':' && (s.p[2] == '\\' || s.p[2] == '/') &&
        (s.p[8] == '\\' || s.p[8] == '/')) {
        for (k = 0; k < 5 && snx_to_lower(s.p[3 + k]) == "users"[k]; k++) {}
        if (k == 5) i = 9;
    }
    if (!i && starts(s, "/data/user/")) {
        i = 11 + dec_run(skip(s, 11));
        if (i == 11 || i >= s.n || s.p[i] != '/') return 0;
        i++;
    }
    if (!i || i >= s.n || s.p[i] == '/' || s.p[i] == '\\' || snx_is_blank(s.p[i])) return 0;
    while (i < s.n && s.p[i] != '/' && s.p[i] != '\\' && !snx_is_blank(s.p[i])) i++;
    return i;
}

/* room for n more bytes in the pool: 0, or -1 out of memory, the pool as it was. Its offsets are unsigned */
static int reserve(SnxLogScan* s, size_t n) {
    size_t cap;
    if (s->used + n <= s->cap) return 0;
    if (n > 0x7fffffffu - s->used) return -1;
    cap = s->cap ? s->cap : 4096;
    while (cap < s->used + n) cap *= 2;
    char* grown = (char*)realloc(s->pool, cap);
    if (!grown) return -1;
    s->pool = grown;
    s->cap = cap;
    return 0;
}

/* text into the pool as it goes into a report, its end's blanks taken off: a home folder as ~, invalid UTF-8 as
   U+FFFD, cut at SNX_LOGSCAN_LINE bytes (on a character's boundary) with "...". Written in place: each step adds 4
   bytes at most, so SNX_LOGSCAN_LINE + 8 is room enough. Its offset, 0 for an empty text or out of memory */
static unsigned add(SnxLogScan* s, Span text) {
    size_t i = 0, n = 0, len, home;
    while (text.n && (snx_is_blank(text.p[text.n - 1]) || text.p[text.n - 1] == '\r')) text.n--;
    if (blank(text) || reserve(s, SNX_LOGSCAN_LINE + 8)) return 0;
    char* out = s->pool + s->used;
    while (i < text.n) {
        if (n >= SNX_LOGSCAN_LINE) {
            memcpy(out + n, "...", 3);
            n += 3;
            break;
        }
        home = home_at(skip(text, i));
        if (home) {
            out[n++] = '~';
            i += home;
            continue;
        }
        len = utf8_len((const unsigned char*)text.p + i, text.n - i);
        if (len)
            memcpy(out + n, text.p + i, len);
        else
            memcpy(out + n, "\xef\xbf\xbd", 3);
        n += len ? len : 3;
        i += len ? len : 1;
    }
    out[n] = 0;
    s->used += n + 1;
    return (unsigned)(out - s->pool);
}
static unsigned add_c(SnxLogScan* s, const char* text) { return add(s, cstr(text)); }

const char* snx_logscan_text(const SnxLogScan* s, unsigned at) { return at && s->pool ? s->pool + at : ""; }

char* snx_logscan_tilde(const char* text, size_t n, size_t* size) {
    char* out = (char*)malloc(n + 1); /* a home becomes one byte: never longer */
    size_t i = 0, o = 0, home;
    if (!out) return NULL;
    while (i < n) {
        home = home_at(span(text + i, n - i));
        if (home) {
            out[o++] = '~';
            i += home;
        } else
            out[o++] = text[i++];
    }
    out[o] = 0;
    *size = o;
    return out;
}

/* ============================================================ lines */
/* "<name+off>": the function or module and the offset after the last + when that is hex */
static SnxLogSym symbol(SnxLogScan* s, Span text) {
    SnxLogSym sym;
    size_t plus = text.n;
    sym.name = sym.offset = 0;
    while (plus && text.p[plus - 1] != '+') plus--;
    if (plus && plus < text.n && hex_run(skip(text, plus)) == text.n - plus) {
        sym.name = add(s, span(text.p, plus - 1));
        sym.offset = add(s, skip(text, plus));
    } else
        sym.name = add(s, text);
    return sym;
}

/* "   #3 1683a358 <name+38>", a backtrace's frame */
static int frame(SnxLogScan* s, Span line, SnxLogFrame* f) {
    size_t i = space_run(line);
    if (!i || i >= line.n || line.p[i] != '#') return 0;
    size_t d = dec_run(skip(line, i + 1));
    if (!d || i + 1 + d >= line.n || line.p[i + 1 + d] != ' ') return 0;
    Span rest = skip(line, i + 2 + d);
    size_t h = hex_run(rest);
    if (!h) return 0;
    f->address = add(s, span(rest.p, h));
    f->sym.name = f->sym.offset = 0;
    rest = trim(skip(rest, h));
    if (!rest.n) return 1;
    if (rest.p[0] != '<' || rest.p[rest.n - 1] != '>') return 0;
    f->sym = symbol(s, span(rest.p + 1, rest.n - 2));
    return 1;
}

/* "  pc <sym>  lr <sym>" (kernel_dump_regs), what an older build printed more after; the pc's symbol through *pc */
static int pc_line(Span line, Span* pc) {
    size_t i = space_run(line);
    if (!i || !starts(skip(line, i), "pc ")) return 0;
    Span rest = skip(line, i + 3);
    *pc = span(rest.p, 0);
    if (rest.n && rest.p[0] == '<') {
        long end = find(rest, ">");
        if (end < 0) return 0;
        *pc = span(rest.p + 1, (size_t)end - 1);
        rest = skip(rest, (size_t)end + 1);
    }
    i = space_run(rest);
    return i && starts(skip(rest, i), "lr ");
}

/* the report's lines holding the game's bytes or its registers, never copied out */
static int dump(Span line) {
    static const char* const prefixes[] = {
        "code ", "mem ", "vtable:", "-> ", "pc=", "cpsr=", "info+", "data:", "(the rest"};
    size_t i = space_run(line), k;
    Span rest = skip(line, i);
    if (!i) return 0;
    for (k = 0; k < sizeof prefixes / sizeof prefixes[0]; k++)
        if (starts(rest, prefixes[k])) return 1;
    if (rest.n && (rest.p[0] == 'x' || rest.p[0] == 'r')) { /* x12= and r3 = */
        k = dec_run(skip(rest, 1));
        if (k) {
            rest = skip(rest, 1 + k);
            rest = skip(rest, space_run(rest));
            return rest.n && rest.p[0] == '=';
        }
    }
    return 0;
}

/* ============================================================ the failure markers */
typedef struct {
    int kind;
    Span signal, game_address, reason, result, imm, address, word, set, what, code, fatal;
} Marker;

/* hex digits at the start of s, and s moved past them; 0 when there are none */
static int take_hex(Span* s, Span* out) {
    size_t h = hex_run(*s);
    if (!h) return 0;
    *out = span(s->p, h);
    *s = skip(*s, h);
    return 1;
}
static int take_lit(Span* s, const char* lit) {
    if (!starts(*s, lit)) return 0;
    *s = skip(*s, strlen(lit));
    return 1;
}
/* s from just after lit's first place in it */
static int after(Span* s, const char* lit) {
    long at = find(*s, lit);
    if (at < 0) return 0;
    *s = skip(*s, (size_t)at + strlen(lit));
    return 1;
}

static int marker(Span line, Marker* m) {
    Span s = line, t;
    long at;
    *m = (Marker){0};
    if (after(&s, "*** Crash (")) {
        if (take_lit(&s, "signal ")) {
            size_t d = dec_run(s);
            if (!d) return 0;
            m->signal = span(s.p, d);
        } else if (starts(s, "exception 0x")) {
            s = skip(s, 10);
            m->signal = span(s.p, 2 + hex_run(skip(s, 2)));
        } else
            return 0;
        t = s;
        if (after(&t, "(game address ")) take_hex(&t, &m->game_address);
        m->kind = SNX_LOG_CRASH;
        return 1;
    }
    s = line;
    if (after(&s, "*** The game called svcBreak (reason")) {
        if (!s.n || (s.p[0] != ' ' && s.p[0] != '=')) return 0;
        s = skip(s, 1);
        if (!take_hex(&s, &m->reason)) return 0;
        m->kind = SNX_LOG_SVCBREAK;
        return 1;
    }
    s = line;
    if (after(&s, "*** The game returned from an exception it had no handler for (")) {
        if (!take_hex(&s, &m->result)) return 0;
        m->kind = SNX_LOG_EXCEPTION;
        return 1;
    }
    s = line;
    if (after(&s, "*** The game stopped at BRK #")) {
        if (!take_hex(&s, &m->imm) || !take_lit(&s, " at ") || !take_hex(&s, &m->address)) return 0;
        m->kind = SNX_LOG_BRK;
        return 1;
    }
    s = line;
    if (after(&s, "*** The game ran instruction ")) {
        if (!take_hex(&s, &m->word) || !take_lit(&s, " at ") || !take_hex(&s, &m->address) ||
            !starts(s, ", which the recompiler cannot run"))
            return 0;
        m->set = cstr("A64");
        m->kind = SNX_LOG_UNDEFINED;
        return 1;
    }
    s = line;
    if (take_lit(&s, "Unsupported ")) {
        if (take_lit(&s, "Thumb"))
            m->set = cstr("Thumb");
        else if (take_lit(&s, "A32"))
            m->set = cstr("A32");
        else
            return 0;
        if (!take_lit(&s, " instruction ") || !take_hex(&s, &m->word) || !take_lit(&s, " at ") ||
            !take_hex(&s, &m->address))
            return 0;
        m->kind = SNX_LOG_UNDEFINED;
        return 1;
    }
    s = line;
    if (after(&s, "*** The game's thread ")) {
        s = skip(s, dec_run(s));
        if (!take_lit(&s, " stopped at ") || !take_hex(&s, &m->address) || !take_lit(&s, ": ")) return 0;
        at = find(s, " ***");
        if (at < 0) return 0;
        s.n = (size_t)at;
        if (!s.n || s.p[s.n - 1] != ')') return 0;
        for (at = (long)s.n - 1; at > 0 && s.p[at] != '('; at--) {}
        if (at < 1 || s.p[at - 1] != ' ') return 0;
        m->what = span(s.p, (size_t)at - 1);
        m->code = span(s.p + at + 1, s.n - (size_t)at - 2);
        m->kind = SNX_LOG_THREAD_STOPPED;
        return 1;
    }
    s = line;
    if (after(&s, "*** Shutdown: ")) {
        at = find(s, " ***");
        if (at < 0) return 0;
        m->what = span(s.p, (size_t)at);
        m->kind = SNX_LOG_HOST_ABORT;
        return 1;
    }
    s = line;
    if (take_lit(&s, "FATAL: ")) {
        m->fatal = s;
        m->kind = SNX_LOG_FATAL;
        return 1;
    }
    return 0;
}

/* ============================================================ the scan */
/* A game log is all of itself. The launcher's Share Log file (src/gui/main.c's share_log) starts with snx_build_info's
   line, "ScarletNX <branch> <commit> (<compiler>, <system>)", then its message and sections: the last run's output
   under "== the last run: <path> ==" and the newest game log under "== <path> ==". The game log's lines are scanned,
   else the run's output when it had none */
enum { PHASE_START, PHASE_LOG, PHASE_HEAD, PHASE_RUN, PHASE_GAME, PHASE_DONE };

/* "ScarletNX - <title>", what an older build followed with " (<folder>)", which is cut */
static void header(SnxLogScan* s, Span title) {
    size_t i;
    for (i = title.n; i > 2; i--)
        if (title.p[i - 1] == '(' && title.p[i - 2] == ' ') {
            Span path = skip(title, i);
            if (path.n && title.p[title.n - 1] == ')' &&
                (path.p[0] == '/' || path.p[0] == '~' ||
                 (path.n > 2 && snx_is_alpha(path.p[0]) && path.p[1] == ':' &&
                  (path.p[2] == '\\' || path.p[2] == '/'))))
                title.n = i - 2;
            break;
        }
    s->title = add(s, title);
}

/* "build: ScarletNX <branch> <commit> (<compiler>, <system>)" or the launcher's line without "build: " */
static int build_line(SnxLogScan* s, Span line) {
    Span rest = line;
    if (!take_lit(&rest, "ScarletNX ") || !rest.n || rest.p[rest.n - 1] != ')') return 0;
    long sp = find(rest, " ");
    if (sp <= 0) return 0;
    Span branch = span(rest.p, (size_t)sp);
    rest = skip(rest, (size_t)sp + 1);
    sp = find(rest, " (");
    if (sp <= 0) return 0;
    Span commit = span(rest.p, (size_t)sp);
    s->build = add(s, line);
    s->branch = add(s, branch);
    s->commit = add(s, commit);
    return 1;
}

static int ipc_line(SnxLogScan* s, Span line) {
    static const char* const kinds[] = {"[bad request] ", "[undeclared] ", "[unknown] "};
    Span rest = line, iface;
    int kind = -1;
    long at;
    unsigned cmd = 0;
    if (take_lit(&rest, "ipc: ")) { /* "ipc: <name>: control request <n>, UnknownCommandId" */
        at = find(rest, ": control request ");
        if (at <= 0 || find(rest, ", UnknownCommandId") < 0) return 0;
        iface = span(rest.p, (size_t)at);
        rest = skip(rest, (size_t)at + 18);
        kind = SNX_IPC_UNKNOWN_CONTROL;
    } else {
        if (!space_run(rest)) return 0;
        rest = skip(rest, space_run(rest));
        for (int k = 0; k < 3 && kind < 0; k++)
            if (take_lit(&rest, kinds[k])) kind = k;
        if (kind < 0) return 0;
        at = find(rest, " cmd ");
        if (at <= 0) return 0;
        iface = span(rest.p, (size_t)at);
        rest = skip(rest, (size_t)at + 5);
    }
    size_t d = dec_run(rest);
    if (!d || d > 9) return 0;
    for (unsigned i = 0; i < d; i++) cmd = cmd * 10 + (unsigned)(rest.p[i] - '0');
    for (int k = 0; k < s->nipc; k++) {
        const char* name = snx_logscan_text(s, s->ipc[k].iface);
        if (s->ipc[k].kind == kind && s->ipc[k].command == cmd && strlen(name) == iface.n &&
            !memcmp(name, iface.p, iface.n)) {
            s->ipc[k].count++;
            return 1;
        }
    }
    if (s->nipc < SNX_LOGSCAN_IPC) {
        s->ipc[s->nipc].kind = kind;
        s->ipc[s->nipc].iface = add(s, iface);
        s->ipc[s->nipc].command = cmd;
        s->ipc[s->nipc].count = 1;
        s->nipc++;
    }
    return 1;
}

/* by kind, interface and command, as the summary lists them (an insertion sort: there are SNX_LOGSCAN_IPC at most) */
static void sort_ipc(SnxLogScan* s) {
    for (int i = 1; i < s->nipc; i++)
        for (int j = i; j > 0; j--) {
            SnxLogIpc a = s->ipc[j - 1], b = s->ipc[j];
            int c =
                a.kind != b.kind ? a.kind - b.kind : strcmp(snx_logscan_text(s, a.iface), snx_logscan_text(s, b.iface));
            if (c < 0 || (!c && a.command <= b.command)) break;
            s->ipc[j - 1] = b;
            s->ipc[j] = a;
        }
}

static void warning(SnxLogScan* s, Span line) {
    size_t mark = s->used;
    int k;
    if (!warning_word(line)) return;
    unsigned at = add(s, line);
    if (!at) return;
    for (k = 0; k < s->nwarnings; k++)
        if (!strcmp(snx_logscan_text(s, s->warnings[k].message), s->pool + at)) {
            s->warnings[k].count++;
            s->used = mark; /* the copy goes again */
            return;
        }
    if (s->nwarnings < SNX_LOGSCAN_WARNINGS) {
        s->warnings[s->nwarnings].message = at;
        s->warnings[s->nwarnings].count = 1;
        s->nwarnings++;
    } else
        s->used = mark;
}

/* the number of decimal digits at the start of s, 0 without any */
static unsigned long number(Span s) {
    unsigned long v = 0;
    size_t d = dec_run(s);
    for (size_t i = 0; i < d && i < 18; i++) v = v * 10 + (unsigned long)(s.p[i] - '0');
    return v;
}
static void keep_line(char* raw, unsigned* len, Span line) {
    size_t n = line.n < SNX_LOGSCAN_RAW ? line.n : SNX_LOGSCAN_RAW;
    memcpy(raw, line.p, n);
    *len = (unsigned)n;
}

/* A gap of kind, what its text, counted again when the same one comes back (compared as the pool keeps it, as a
   warning is). 1: the line is taken either way */
static int gap(SnxLogScan* s, int kind, Span what) {
    size_t mark = s->used;
    unsigned at = add(s, what);
    int k;
    if (!at) return 1;
    for (k = 0; k < s->ngaps; k++)
        if (s->gaps[k].kind == kind && !strcmp(snx_logscan_text(s, s->gaps[k].what), s->pool + at)) {
            s->gaps[k].count++;
            s->used = mark;
            return 1;
        }
    if (s->ngaps < SNX_LOGSCAN_GAPS) {
        s->gaps[s->ngaps].kind = kind;
        s->gaps[s->ngaps].what = at;
        s->gaps[s->ngaps].count = 1;
        s->ngaps++;
    } else
        s->used = mark;
    return 1;
}
/* "<a>: <b>" as one gap, for a reason that goes with the macro it is about */
static int gap_pair(SnxLogScan* s, int kind, Span a, Span b) {
    char text[SNX_LOGSCAN_LINE + 1];
    int n = snprintf(text, sizeof text, "%.*s: %.*s", (int)(a.n < SNX_LOGSCAN_LINE ? a.n : SNX_LOGSCAN_LINE), a.p,
                     (int)(b.n < SNX_LOGSCAN_LINE ? b.n : SNX_LOGSCAN_LINE), b.p);
    return gap(s, kind, span(text, n < 0 ? 0 : (size_t)n < sizeof text ? (size_t)n : sizeof text - 1));
}

/* What the runtime missed, in the words it logs it: a shader the translator can't express (gpu/vk/pipeline.c,
   compute.c), methods of a class no engine runs and the GPU operations it skips (gpu.c, maxwell/engines.c,
   fermi2d.c), a macro with no native form, one the compiler can't follow and an operation the interpreter doesn't
   know (maxwell/mme.c, mme_jit.c), an SVC with no handler or an argument it doesn't know (hle/svc/svc.c, svc_mem.c),
   and the guest code the recompiler missed (runtime/guest_jit.c). 1 when line is one of them */
static int gap_line(SnxLogScan* s, Span line) {
    Span rest = line;
    long at;
    switch (line.p[0]) {
        case 'v': /* "vulkan: shader <hash> (stage <n>) not translated: <why>", "vulkan: compute <hash> ..." */
            if (!take_lit(&rest, "vulkan: ") || (at = find(rest, " not translated: ")) < 0) return 0;
            if (starts(rest, "shader ")) return gap(s, SNX_GAP_SHADER, skip(rest, (size_t)at + 17));
            if (starts(rest, "compute ")) return gap(s, SNX_GAP_COMPUTE, skip(rest, (size_t)at + 17));
            return 0;
        case 'g':
            if (!take_lit(&rest, "gpu: ")) return 0;
            if (take_lit(&rest,
                         "method ")) { /* "<m> for class <c> on subchannel <n>: no engine here runs that class" */
                at = find(rest, " for class ");
                if (at < 0 || !ends(rest, ": no engine here runs that class")) return 0;
                rest = skip(rest, (size_t)at + 11);
                return gap(s, SNX_GAP_CLASS, span(rest.p, hex_run(rest)));
            }
            if (starts(rest, "macro ") && ends(rest, " is unknown")) return gap(s, SNX_GAP_MME, rest);
            if (ends(rest, " is unknown") || ends(rest, " isn't done") || find(rest, " is not emulated") >= 0 ||
                find(rest, " is skipped") >= 0)
                return gap(s, SNX_GAP_GPU, rest);
            return 0;
        case 'm':
            if (!take_lit(&rest, "mme: ")) return 0;
            if (take_lit(&rest, "macros are interpreted: ")) /* no compiler at all */
                return gap_pair(s, SNX_GAP_MACRO_COMPILE, cstr("every macro"), rest);
            if (starts(rest, "macro ") && (at = find(rest, " (hash ")) > 0 && find(rest, ") is interpreted: ") > at) {
                /* "macro <n> (hash <h>) is interpreted: <why>" */
                Span hash = skip(rest, (size_t)at + 7);
                hash.n = hex_run(hash);
                return gap_pair(s, SNX_GAP_MACRO_COMPILE, hash,
                                skip(rest, (size_t)find(rest, ") is interpreted: ") + 18));
            }
            if (starts(rest, "macro ") && ends(rest, ": interpreted") && (at = find(rest, ", hash ")) > 0) {
                /* "macro <n> at word <w>, <n> words uploaded together, hash <h>: interpreted": no native form */
                Span hash = skip(rest, (size_t)at + 7);
                hash.n = hex_run(hash);
                return gap(s, SNX_GAP_MACRO, hash);
            }
            if (dec_run(rest) && starts(skip(rest, dec_run(rest)), " macro calls, ")) {
                keep_line(s->mme_raw, &s->mme_len, rest);
                return 1;
            }
            if (starts(rest, "the macros interpreted most, by hash: ")) {
                keep_line(s->mme_most_raw, &s->mme_most_len, skip(rest, 38));
                return 1;
            }
            return 0;
        case 's':
            if (take_lit(&rest, "svc ") && (at = find(rest, "): not implemented")) > 0) /* "svc <n> (<name>): ..." */
                return gap(s, SNX_GAP_SVC, span(line.p, (size_t)at + 5));
            if (starts(line, "svcGetInfo ") && (at = find(line, ": unknown, InvalidEnumValue")) > 0)
                return gap(s, SNX_GAP_CPU, span(line.p, (size_t)at));
            return 0;
        case 'j':
            if (!take_lit(&rest, "jit: ")) return 0;
            if (starts(rest, "guest code the recompiler missed ") && (at = find(rest, ". ")) > 0) {
                /* "...on <n> threads. <n> pcs of earlier runs queued" */
                rest = skip(rest, (size_t)at + 2);
                if (!dec_run(rest) || !starts(skip(rest, dec_run(rest)), " pcs of earlier runs")) return 0;
                s->jit_earlier = number(rest);
                s->jit |= 1;
                return 1;
            }
            if (dec_run(rest) && starts(skip(rest, dec_run(rest)), " batches compiled ")) {
                /* "<n> batches compiled <n> pcs (<n> failed, <n> still queued, ..." */
                rest = skip(rest, dec_run(rest) + 18);
                s->jit_compiled = number(rest);
                if ((at = find(rest, " pcs (")) < 0) return 0;
                rest = skip(rest, (size_t)at + 6);
                s->jit_failed = number(rest);
                if ((at = find(rest, " failed, ")) >= 0) s->jit_queued = number(skip(rest, (size_t)at + 9));
                s->jit |= 2;
                return 1;
            }
            return 0;
    }
    return 0;
}

/* by kind, then the most first (an insertion sort: there are SNX_LOGSCAN_GAPS at most) */
static void sort_gaps(SnxLogScan* s) {
    for (int i = 1; i < s->ngaps; i++)
        for (int j = i; j > 0; j--) {
            SnxLogGap a = s->gaps[j - 1], b = s->gaps[j];
            if (a.kind < b.kind || (a.kind == b.kind && a.count >= b.count)) break;
            s->gaps[j - 1] = b;
            s->gaps[j] = a;
        }
}

/* A line of the report after the failure's marker: its detail lines (what, where, which thread), the pc's symbol and
   the first backtrace, up to a blank line, another marker or the thread list. 0 once the report ended */
static int report_line(SnxLogScan* s, Span line) {
    static const char* const places[] = {"inside the recompiled code of game function ", "inside the recompiled ",
                                         "in the runtime itself (exe offset +"};
    size_t mark = s->used;
    Span pc, rest;
    SnxLogFrame f;
    if (blank(line) || starts(line, "***") || starts(line, "FATAL:") || starts(line, "threads (")) return 0;
    if (frame(s, line, &f)) {
        if (s->nframes < SNX_LOGSCAN_FRAMES)
            s->frames[s->nframes++] = f;
        else
            s->used = mark; /* a frame past those kept */
        return 1;
    }
    if (s->nframes) { /* the backtrace ended: what follows is dumps, or another thread's */
        if (!dump(line) && !starts(line, "  last ") && !starts(line, "    ")) return 0;
        return 1;
    }
    if (pc_line(line, &pc)) {
        if (pc.n) s->pc = symbol(s, pc);
        return 1;
    }
    if (dump(line) || !starts(line, "  ")) return 1;
    Span text = trim(line);
    if (!s->place) {
        rest = text;
        if (take_lit(&rest, places[0]) && hex_run(rest)) {
            s->place = SNX_PLACE_GAME;
            s->place_detail[0] = add(s, span(rest.p, hex_run(rest)));
        } else if (rest = text,
                   take_lit(&rest, places[1]) && (starts(rest, "Thumb block ") || starts(rest, "A32 block "))) {
            s->place = SNX_PLACE_BLOCK;
            s->place_detail[0] = add(s, span(rest.p, rest.p[0] == 'T' ? 5 : 3));
            rest = skip(rest, rest.p[0] == 'T' ? 12 : 10);
            s->place_detail[1] = add(s, span(rest.p, hex_run(rest)));
        } else if (rest = text, take_lit(&rest, places[2]) && hex_run(rest)) {
            s->place = SNX_PLACE_RUNTIME;
            s->place_detail[0] = add(s, span(rest.p, hex_run(rest)));
        } else if (rest = text, take_lit(&rest, "in ") && find(rest, " +") > 0) {
            /* "in <library> +<offset> (<symbol>), on ..." */
            long sp = find(rest, " +");
            Span lib = span(rest.p, (size_t)sp);
            rest = skip(rest, (size_t)sp + 2);
            Span off = span(rest.p, hex_run(rest));
            long open = find(rest, " (");
            long close = find(rest, ")");
            if (off.n && find(lib, " ") < 0 && open == (long)off.n && close > open) {
                s->place = SNX_PLACE_LIBRARY;
                s->place_detail[0] = add(s, lib);
                s->place_detail[1] = add(s, off);
                s->place_detail[2] = add(s, span(rest.p + open + 2, (size_t)(close - open - 2)));
                if (!s->place_detail[2]) s->place_detail[2] = add_c(s, "?");
            }
        }
    }
    if (s->ndetails < SNX_LOGSCAN_DETAILS) s->details[s->ndetails++] = add(s, text);
    return 1;
}

/* the SDK's abort and assert machinery, between svcBreak and the function that gave up: nn::diag, nn::result's
   unhandled results, the svc wrapper, libc's abort */
static int abort_frame(const char* name) {
    Span n = cstr(name);
    long svc = find(n, "nn3svc");
    return find(n, "nn4diag") >= 0 || find(n, "nn6result6detail") >= 0 || find(n, "UnexpectedDefault") >= 0 ||
           (svc >= 0 && find(skip(n, (size_t)svc), "Break") >= 0) || starts(n, "abort") || starts(n, "_abort") ||
           starts(n, "__abort") || find(n, "__assert") >= 0;
}

/* The function the failure is pinned on: the pc's, else the first frame with a symbol. When the game stopped itself
   (svcBreak, an unhandled exception), the pc is in the SDK's svc wrapper, often named after whatever symbol comes
   before it, so only the frames count, past the abort machinery */
static int culprit(const SnxLogScan* s) {
    int stopped = s->outcome == SNX_LOG_SVCBREAK || s->outcome == SNX_LOG_EXCEPTION, i;
    if (s->pc.name && !stopped) return 1;
    for (i = 0; i < s->nframes; i++)
        if (s->frames[i].sym.name && !(stopped && abort_frame(snx_logscan_text(s, s->frames[i].sym.name))))
            return 2 + i;
    return 0;
}
const SnxLogSym* snx_logscan_function(const SnxLogScan* s) {
    return s->function == 1 ? &s->pc : s->function >= 2 ? &s->frames[s->function - 2].sym : NULL;
}

/* numbers in a message as N, so the same message with other values is the same signature: a word of hex digits
   (optionally after 0x) holding a decimal digit */
static void normalized(Buf* b, const char* text) {
    size_t i = 0, j, k;
    int digit;
    while (text[i]) {
        if (!snx_is_alnum(text[i]) && text[i] != '_') {
            put_n(b, text + i++, 1);
            continue;
        }
        for (j = i; snx_is_alnum(text[j]) || text[j] == '_'; j++) {}
        k = i + (text[i] == '0' && text[i + 1] == 'x' ? 2 : 0);
        digit = 0;
        for (size_t m = k; m < j; m++) digit |= text[m] >= '0' && text[m] <= '9';
        if (k < j && digit && hex_run(span(text + k, j - k)) == j - k)
            put(b, "N");
        else
            put_n(b, text + i, j - i);
        i = j;
    }
}

static const char* const outcome_names[] = {"no failure logged", "crash",      "svcBreak",
                                            "exception",         "brk",        "unsupported instruction",
                                            "thread stopped",    "host abort", "fatal"};
const char* snx_logscan_outcome_name(int outcome) {
    return outcome >= 0 && outcome <= SNX_LOG_FATAL ? outcome_names[outcome] : "?";
}

/* the same for every log that failed the same way: the outcome and the place, a function's mangled name when there is
   one (a module and an offset only within one game and version), the runtime's offset with its build. None where the
   place says nothing (no frame named, a runtime offset of an unknown build, a module's offset of an unknown game): such
   a signature would group reports that have only their kind in common */
static unsigned signature(SnxLogScan* s) {
    Buf b = {0};
    const SnxLogSym* fn = snx_logscan_function(s);
    const char* name;
    unsigned at;
    switch (s->outcome) {
        case SNX_LOG_UNDEFINED:
            put(&b, "unsupported ");
            put(&b, snx_logscan_text(s, s->set));
            put(&b, " instruction ");
            put(&b, snx_logscan_text(s, s->word));
            break;
        case SNX_LOG_FATAL:
            put(&b, "fatal ");
            normalized(&b, snx_logscan_text(s, s->fatal));
            break;
        case SNX_LOG_HOST_ABORT:
            put(&b, "host ");
            normalized(&b, snx_logscan_text(s, s->what));
            break;
        default:
            if (s->outcome == SNX_LOG_CRASH) {
                put(&b, "crash (");
                put(&b, s->ndetails ? snx_logscan_text(s, s->details[0]) : "unknown");
                put(&b, ")");
            } else if (s->outcome == SNX_LOG_EXCEPTION) {
                put(&b, "exception ");
                put(&b, snx_logscan_text(s, s->result));
            } else if (s->outcome == SNX_LOG_THREAD_STOPPED)
                put(&b, snx_logscan_text(s, s->what));
            else
                put(&b, s->outcome == SNX_LOG_BRK ? "brk" : "svcBreak");
            /* in the runtime or a library, the host place is the bug's, whatever game code led there */
            if (s->place == SNX_PLACE_RUNTIME) {
                if (!s->commit) goto none;
                put(&b, " in the runtime ");
                put(&b, snx_logscan_text(s, s->commit));
                put(&b, " +");
                put(&b, snx_logscan_text(s, s->place_detail[0]));
            } else if (s->place == SNX_PLACE_LIBRARY) {
                put(&b, " in ");
                put(&b, snx_logscan_text(s, s->place_detail[0]));
                put(&b, " ");
                put(&b, snx_logscan_text(s, s->place_detail[2]));
            } else if (fn) {
                name = snx_logscan_text(s, fn->name);
                put(&b, " in ");
                if (strcmp(name, "rtld") && strcmp(name, "main") && strcmp(name, "sdk") && strcmp(name, "module") &&
                    strncmp(name, "subsdk", 6))
                    put(&b, name);
                else {
                    if (!s->title_id && !s->title) goto none;
                    put(&b, s->title_id ? snx_logscan_text(s, s->title_id) : snx_logscan_text(s, s->title));
                    put(&b, " ");
                    put(&b, name);
                    put(&b, "+");
                    put(&b, snx_logscan_text(s, fn->offset));
                }
            } else
                goto none;
    }
    if (b.failed || !b.p) goto none;
    at = add_c(s, b.p);
    free(b.p);
    return at;
none:
    free(b.p);
    return 0;
}

/* The renderer's frame lines (gpu/vk/renderer.c): "vulkan: a <ms> ms frame: <what it did>" for each frame over 50 ms,
   and every 600 presents "...; <n> frames over 50 ms in the last 600, the slowest <ms> ms" and "vulkan: memory used
   and budgeted: ...". 1 when line is one of them */
static int frame_line(SnxLogScan* s, Span line) {
    static const char over[] = " frames over 50 ms in the last 600, the slowest ";
    Span rest = line;
    if (take_lit(&rest, "vulkan: a ") && dec_run(rest) && starts(skip(rest, dec_run(rest)), " ms frame: ")) {
        unsigned long ms = number(rest);
        int k, slot = s->nslow;
        s->slow_since++;
        if (ms > s->slowest_ms) s->slowest_ms = ms;
        if (s->nslow == SNX_LOGSCAN_SLOW) /* the fastest kept goes, when this one is slower */
            for (slot = -1, k = 0; k < SNX_LOGSCAN_SLOW; k++)
                if (s->slow_ms[k] < ms && (slot < 0 || s->slow_ms[k] < s->slow_ms[slot])) slot = k;
        if (slot >= 0) {
            keep_line(s->slow_raw[slot], &s->slow_len[slot], skip(line, 8));
            s->slow_ms[slot] = ms;
            if (slot == s->nslow) s->nslow++;
        }
        return 1;
    }
    if (starts(line, "vulkan: memory used and budgeted:")) {
        keep_line(s->memory_raw, &s->memory_len, skip(line, 8));
        return 1;
    }
    long at = find(line, over);
    if (at > 0 && starts(line, "vulkan: ")) {
        size_t d = (size_t)at;
        unsigned long ms = number(skip(line, (size_t)at + sizeof over - 1));
        while (d && line.p[d - 1] >= '0' && line.p[d - 1] <= '9') d--;
        s->slow_counted += number(skip(line, d));
        s->slow_since = 0; /* the lines since were counted in it */
        if (ms > s->slowest_ms) s->slowest_ms = ms;
        keep_line(s->frame_stats_raw, &s->frame_stats_len, skip(line, 8));
        return 1;
    }
    return 0;
}

/* the launcher's section lines: "== <anything> ==" */
static int section(Span line) { return line.n >= 6 && starts(line, "== ") && !memcmp(line.p + line.n - 3, " ==", 3); }

/* a line into one of the rings of newest lines, cut at SNX_LOGSCAN_RAW */
static void keep_raw(char (*raw)[SNX_LOGSCAN_RAW], unsigned* len, unsigned long* seen, int slots, Span line) {
    size_t n = line.n < SNX_LOGSCAN_RAW ? line.n : SNX_LOGSCAN_RAW;
    int slot = (int)(*seen % (unsigned long)slots);
    memcpy(raw[slot], line.p, n);
    len[slot] = (unsigned)n;
    ++*seen;
}

/* what the run section said goes when the game log's section begins: the launcher's fields stay */
static void restart(SnxLogScan* s) {
    unsigned build = s->launcher_build, message = s->launcher_message;
    char* pool = s->pool;
    size_t cap = s->cap, mark = s->mark;
    unsigned long seen = s->seen;
    memset(s, 0, sizeof *s);
    s->pool = pool;
    s->cap = cap;
    s->used = s->mark = mark;
    s->seen = seen;
    s->launcher_build = build;
    s->launcher_message = message;
    s->centiseconds = -1;
    s->phase = PHASE_GAME;
}

int snx_logscan_begin(SnxLogScan* s) {
    memset(s, 0, sizeof *s);
    s->centiseconds = -1;
    if (reserve(s, 4096)) return -1;
    s->pool[0] = 0; /* offset 0, the empty text */
    s->used = 1;
    return 0;
}

/* One line of the log. Each test is a prefix or a byte search, the failure markers only for a line with "***" (or the
   two that start a line), so a line costs a pass or two over its bytes */
void snx_logscan_line(SnxLogScan* s, const char* p, size_t n) {
    Span line = span(p, n), rest;
    Marker m;
    long cs;
    if (line.n && line.p[line.n - 1] == '\r') line.n--;
    s->seen++;
    switch (s->phase) {
        case PHASE_START:
            s->phase = PHASE_LOG;
            rest = line;
            if (take_lit(&rest, "ScarletNX ") && !starts(rest, "- ") && line.n && line.p[line.n - 1] == ')') {
                s->launcher_build = add(s, line);
                s->phase = PHASE_HEAD;
                return;
            }
            break;
        case PHASE_HEAD:
            if (s->seen == 2 && starts(line, "Message: ")) s->launcher_message = add(s, skip(line, 9));
            if (section(line) && starts(line, "== the last run:")) {
                s->phase = PHASE_RUN;
                s->mark = s->used;
            }
            return;
        case PHASE_RUN:
            if (section(line)) {
                if (starts(line, "== no game log =="))
                    s->phase = PHASE_DONE;
                else
                    restart(s);
                return;
            }
            break;
        case PHASE_DONE: return;
    }
    size_t st = stamp(line, &cs);
    if (st) s->centiseconds = cs;
    line = skip(line, st);
    if (!blank(line) && !dump(line)) keep_raw(s->tail_raw, s->tail_len, &s->tail_seen, SNX_LOGSCAN_TAIL, line);
    if (s->report_left && (!report_line(s, line) || !--s->report_left)) s->report_left = 0;
    if (s->outcome && !s->fatal && starts(line, "FATAL: ")) s->fatal = add(s, skip(line, 7));
    if (!line.n) return;
    rest = line;
    switch (line.p[0]) {
        case 'S':
            if (!s->title && take_lit(&rest, "ScarletNX - ")) {
                header(s, rest);
                return;
            }
            break;
        case 'b':
            if (!s->build && take_lit(&rest, "build: ") && build_line(s, rest)) return;
            break;
        case 's': /* "system: <OS and version>, <CPU>, <n> threads, <n> GB RAM" (plat_system_info) */
            if (!s->system && take_lit(&rest, "system: ") && rest.n) {
                s->system = add(s, rest);
                return;
            }
            break;
        case 'g':
            if (!s->title_id && take_lit(&rest, "game: ") && hex_run(rest) == 16 &&
                starts(skip(rest, 16), ", version ")) {
                char id[17];
                for (int k = 0; k < 16; k++) id[k] = (char)snx_to_upper(rest.p[k]);
                id[16] = 0;
                s->title_id = add_c(s, id);
                s->game_version = add(s, skip(rest, 26));
                return;
            }
            break;
        case 'v':
            if (frame_line(s, line)) return;
            if (!s->gpu && take_lit(&rest, "vulkan: ") && find(rest, ", Vulkan ") > 0 && find(rest, ", driver ") > 0) {
                long v = find(rest, ", Vulkan "), d = find(rest, ", driver ");
                Span drv = skip(rest, (size_t)d + 9);
                if (d > v) {
                    s->gpu = add(s, span(rest.p, (size_t)v));
                    s->vulkan = add(s, span(rest.p + v + 9, (size_t)(d - v - 9)));
                    drv.n = find(drv, ",") >= 0 ? (size_t)find(drv, ",") : drv.n;
                    s->driver = add(s, drv);
                    return;
                }
            }
            break;
        case '[':
            if (take_lit(&rest, "[game] ")) {
                keep_raw(s->output_raw, s->output_len, &s->output_seen, SNX_LOGSCAN_OUTPUT, rest);
                return;
            }
            break;
        case ' ':
        case '\t':
        case 'i':
            if (ipc_line(s, line)) return;
            break;
    }
    if (gap_line(s, line)) return;
    if (!s->outcome && (find(line, "***") >= 0 || starts(line, "Unsupported ") || starts(line, "FATAL: ")) &&
        marker(line, &m)) {
        s->outcome = m.kind;
        s->signal = add(s, m.signal);
        s->game_address = add(s, m.game_address);
        s->reason = add(s, m.reason);
        s->result = add(s, m.result);
        s->imm = add(s, m.imm);
        s->address = add(s, m.address);
        s->word = add(s, m.word);
        s->set = add(s, m.set);
        s->what = add(s, m.what);
        s->code = add(s, m.code);
        s->fatal = add(s, m.fatal);
        s->report_left = 400;
        return;
    }
    if (!snx_is_blank(line.p[0])) warning(s, line);
}

/* the rings of newest lines into the summary's texts, oldest first */
static int from_ring(SnxLogScan* s, char (*raw)[SNX_LOGSCAN_RAW], const unsigned* len, unsigned long seen, int slots,
                     unsigned* out) {
    unsigned long k = seen > (unsigned long)slots ? seen - (unsigned long)slots : 0;
    int count = 0;
    for (; k < seen; k++) {
        int slot = (int)(k % (unsigned long)slots);
        out[count] = add(s, span(raw[slot], len[slot]));
        if (out[count]) count++;
    }
    return count;
}

void snx_logscan_end(SnxLogScan* s) {
    sort_ipc(s);
    sort_gaps(s);
    /* the macro calls' line only when some were interpreted: "..., <n> compiled and <n> interpreted" */
    if (!ends(span(s->mme_raw, s->mme_len), " and 0 interpreted")) s->mme = add(s, span(s->mme_raw, s->mme_len));
    s->mme_most = add(s, span(s->mme_most_raw, s->mme_most_len));
    s->noutput = from_ring(s, s->output_raw, s->output_len, s->output_seen, SNX_LOGSCAN_OUTPUT, s->output);
    s->ntail = from_ring(s, s->tail_raw, s->tail_len, s->tail_seen, SNX_LOGSCAN_TAIL, s->tail);
    for (int i = 0; i < s->nslow; i++) s->slow_line[i] = add(s, span(s->slow_raw[i], s->slow_len[i]));
    s->frame_stats = add(s, span(s->frame_stats_raw, s->frame_stats_len));
    s->memory = add(s, span(s->memory_raw, s->memory_len));
    if (!s->build && s->launcher_build) build_line(s, cstr(snx_logscan_text(s, s->launcher_build)));
    if (s->outcome) {
        s->function = culprit(s);
        s->signature = signature(s);
    }
}

int snx_logscan(SnxLogScan* s, const char* text, size_t size) {
    const char *p = text, *end = text + size, *nl;
    if (snx_logscan_begin(s)) return -1;
    while (p < end) {
        nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        if (!nl) nl = end;
        snx_logscan_line(s, p, (size_t)(nl - p));
        p = nl + 1;
    }
    snx_logscan_end(s);
    return 0;
}

#define CHUNK     (1 << 20)
#define LINE_KEEP (64 << 10)
int snx_logscan_file(SnxLogScan* s, FILE* f) {
    char* nl;
    size_t have = 0, got, start, end;
    int over = 0; /* the line being read went past LINE_KEEP: its first part was scanned, the rest is dropped */
    if (snx_logscan_begin(s)) return -1;
    char* buf = (char*)malloc(LINE_KEEP + CHUNK);
    if (!buf) return -1;
    while ((got = fread(buf + have, 1, CHUNK, f)) > 0) {
        start = 0;
        end = have + got;
        while ((nl = (char*)memchr(buf + start, '\n', end - start)) != NULL) {
            if (!over) snx_logscan_line(s, buf + start, (size_t)(nl - buf) - start);
            over = 0;
            start = (size_t)(nl - buf) + 1;
        }
        have = end - start;
        if (over)
            have = 0;
        else if (have > LINE_KEEP) {
            snx_logscan_line(s, buf + start, LINE_KEEP);
            over = 1;
            have = 0;
        } else
            memmove(buf, buf + start, have);
    }
    if (have) snx_logscan_line(s, buf, have);
    free(buf);
    if (ferror(f)) return -1;
    snx_logscan_end(s);
    return 0;
}

void snx_logscan_free(SnxLogScan* s) {
    free(s->pool);
    memset(s, 0, sizeof *s);
}

/* the kept slow frames' indexes, the slowest first */
static void slowest_first(const SnxLogScan* s, int* order) {
    int t;
    for (int i = 0; i < s->nslow; i++) order[i] = i;
    for (int i = 1; i < s->nslow; i++)
        for (int j = i; j > 0 && s->slow_ms[order[j]] > s->slow_ms[order[j - 1]]; j--) {
            t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
}

/* ============================================================ JSON */
static void json_string(Buf* b, const char* text) {
    char esc[8];
    put(b, "\"");
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            put_n(b, esc, 2);
        } else if (c < 0x20) {
            sprintf(esc, "\\u%04x", c);
            put(b, esc);
        } else
            put_n(b, text, 1);
    }
    put(b, "\"");
}
static void json_text(Buf* b, const SnxLogScan* s, unsigned at) {
    if (at)
        json_string(b, snx_logscan_text(s, at));
    else
        put(b, "null");
}
static void json_field(Buf* b, const SnxLogScan* s, const char* key, unsigned at, int first) {
    put(b, first ? "\"" : ", \"");
    put(b, key);
    put(b, "\": ");
    json_text(b, s, at);
}
static void json_sym(Buf* b, const SnxLogScan* s, const SnxLogSym* sym, SnxDemangle demangle) {
    const char* name;
    if (!sym || !sym->name) {
        put(b, "null");
        return;
    }
    name = snx_logscan_text(s, sym->name);
    char* plain = demangle && !strncmp(name, "_Z", 2) ? demangle(name) : NULL;
    put(b, "{\"name\": ");
    json_string(b, name);
    put(b, ", \"offset\": ");
    json_text(b, s, sym->offset);
    put(b, ", \"demangled\": ");
    json_string(b, plain ? plain : name);
    put(b, "}");
    free(plain);
}
static void json_list(Buf* b, const SnxLogScan* s, const unsigned* items, int n) {
    put(b, "[");
    for (int i = 0; i < n; i++) {
        if (i) put(b, ", ");
        json_text(b, s, items[i]);
    }
    put(b, "]");
}

/* a gap's kind and the area it is in, by kind (SNX_GAP_*) */
static const char* const gap_kinds[] = {
    "shader", "compute shader", "class", "gpu", "macro", "macro compile", "mme", "svc", "cpu"};
/* 0 the GPU, 1 the macros, 2 the CPU */
static int gap_area(int kind) { return kind <= SNX_GAP_GPU ? 0 : kind <= SNX_GAP_MME ? 1 : 2; }
/* the JIT compiled or queued guest code the recompiler missed, in this run or earlier ones */
static int jit_missed(const SnxLogScan* s) {
    return ((s->jit & 1) && s->jit_earlier) || ((s->jit & 2) && (s->jit_compiled || s->jit_failed || s->jit_queued));
}

char* snx_logscan_json(const SnxLogScan* s, SnxDemangle demangle) {
    static const char* const ipc_kinds[] = {"bad request", "undeclared", "unknown", "unknown control"};
    static const char* const place_kinds[] = {"", "game", "block", "runtime", "library"};
    static const char* const area_names[] = {"gpu", "mme", "cpu"};
    Buf b = {0};
    char num[32];
    int n;
    put(&b, "{\"schema\": ");
    put_num(&b, SNX_LOGSCAN_SCHEMA);
    json_field(&b, s, "title", s->title, 0);
    json_field(&b, s, "title_id", s->title_id, 0);
    json_field(&b, s, "game_version", s->game_version, 0);
    json_field(&b, s, "build", s->build, 0);
    json_field(&b, s, "commit", s->commit, 0);
    json_field(&b, s, "branch", s->branch, 0);
    json_field(&b, s, "system", s->system, 0);
    json_field(&b, s, "gpu", s->gpu, 0);
    json_field(&b, s, "vulkan", s->vulkan, 0);
    json_field(&b, s, "driver", s->driver, 0);
    put(&b, ", \"seconds\": ");
    if (s->centiseconds >= 0) {
        sprintf(num, "%ld.%02ld", s->centiseconds / 100, s->centiseconds % 100);
        put(&b, num);
    } else
        put(&b, "null");
    put(&b, ", \"outcome\": ");
    json_string(&b, snx_logscan_outcome_name(s->outcome));
    put(&b, ", \"failure\": ");
    if (s->outcome == SNX_LOG_NONE)
        put(&b, "null");
    else {
        put(&b, "{\"kind\": ");
        json_string(&b, snx_logscan_outcome_name(s->outcome));
        put(&b, ", \"details\": ");
        json_list(&b, s, s->details, s->ndetails);
        json_field(&b, s, "fatal", s->fatal, 0);
        put(&b, ", \"pc\": ");
        json_sym(&b, s, s->pc.name ? &s->pc : NULL, demangle);
        put(&b, ", \"frames\": [");
        for (int i = 0; i < s->nframes; i++) {
            put(&b, i ? ", {\"address\": " : "{\"address\": ");
            json_text(&b, s, s->frames[i].address);
            put(&b, ", \"symbol\": ");
            json_sym(&b, s, s->frames[i].sym.name ? &s->frames[i].sym : NULL, demangle);
            put(&b, "}");
        }
        put(&b, "], \"place\": ");
        if (s->place) {
            put(&b, "{\"kind\": ");
            json_string(&b, place_kinds[s->place]);
            put(&b, ", \"detail\": ");
            n = s->place == SNX_PLACE_LIBRARY ? 3 : s->place == SNX_PLACE_BLOCK ? 2 : 1;
            json_list(&b, s, s->place_detail, n);
            put(&b, "}");
        } else
            put(&b, "null");
        put(&b, ", \"function\": ");
        json_sym(&b, s, snx_logscan_function(s), demangle);
        json_field(&b, s, "signal", s->signal, 0);
        json_field(&b, s, "game_address", s->game_address, 0);
        json_field(&b, s, "reason", s->reason, 0);
        json_field(&b, s, "result", s->result, 0);
        json_field(&b, s, "imm", s->imm, 0);
        json_field(&b, s, "address", s->address, 0);
        json_field(&b, s, "word", s->word, 0);
        json_field(&b, s, "set", s->set, 0);
        json_field(&b, s, "what", s->outcome == SNX_LOG_CRASH && s->ndetails ? s->details[0] : s->what, 0);
        json_field(&b, s, "code", s->code, 0);
        put(&b, "}");
    }
    json_field(&b, s, "signature", s->signature, 0);
    put(&b, ", \"frames\": ");
    if (s->nslow || s->frame_stats) {
        int order[SNX_LOGSCAN_SLOW];
        put(&b, "{\"slow\": ");
        put_num(&b, s->slow_counted + s->slow_since);
        put(&b, ", \"slowest_ms\": ");
        put_num(&b, s->slowest_ms);
        put(&b, ", \"slowest\": [");
        slowest_first(s, order);
        for (int i = 0; i < s->nslow; i++) {
            put(&b, i ? ", {\"ms\": " : "{\"ms\": ");
            put_num(&b, s->slow_ms[order[i]]);
            json_field(&b, s, "line", s->slow_line[order[i]], 0);
            put(&b, "}");
        }
        put(&b, "]");
        json_field(&b, s, "stats", s->frame_stats, 0);
        json_field(&b, s, "memory", s->memory, 0);
        put(&b, "}");
    } else
        put(&b, "null");
    put(&b, ", \"ipc\": [");
    for (int i = 0; i < s->nipc; i++) {
        put(&b, i ? ", {\"kind\": " : "{\"kind\": ");
        json_string(&b, ipc_kinds[s->ipc[i].kind]);
        json_field(&b, s, "interface", s->ipc[i].iface, 0);
        put(&b, ", \"command\": ");
        put_num(&b, s->ipc[i].command);
        put(&b, ", \"count\": ");
        put_num(&b, s->ipc[i].count);
        put(&b, "}");
    }
    put(&b, "], \"missed\": [");
    for (int i = 0; i < s->ngaps; i++) {
        put(&b, i ? ", {\"area\": " : "{\"area\": ");
        json_string(&b, area_names[gap_area(s->gaps[i].kind)]);
        put(&b, ", \"kind\": ");
        json_string(&b, gap_kinds[s->gaps[i].kind]);
        json_field(&b, s, "what", s->gaps[i].what, 0);
        put(&b, ", \"count\": ");
        put_num(&b, s->gaps[i].count);
        put(&b, "}");
    }
    put(&b, "], \"mme\": ");
    if (s->mme || s->mme_most) {
        put(&b, "{");
        json_field(&b, s, "calls", s->mme, 1);
        json_field(&b, s, "interpreted_most", s->mme_most, 0);
        put(&b, "}");
    } else
        put(&b, "null");
    put(&b, ", \"jit\": ");
    if (s->jit) {
        put(&b, "{\"earlier\": ");
        if (s->jit & 1)
            put_num(&b, s->jit_earlier);
        else
            put(&b, "null");
        put(&b, ", \"compiled\": ");
        if (s->jit & 2) {
            put_num(&b, s->jit_compiled);
            put(&b, ", \"failed\": ");
            put_num(&b, s->jit_failed);
            put(&b, ", \"queued\": ");
            put_num(&b, s->jit_queued);
        } else
            put(&b, "null, \"failed\": null, \"queued\": null");
        put(&b, "}");
    } else
        put(&b, "null");
    put(&b, ", \"warnings\": [");
    for (int i = 0; i < s->nwarnings; i++) {
        put(&b, i ? ", {\"message\": " : "{\"message\": ");
        json_text(&b, s, s->warnings[i].message);
        put(&b, ", \"count\": ");
        put_num(&b, s->warnings[i].count);
        put(&b, "}");
    }
    put(&b, "], \"game_output\": ");
    json_list(&b, s, s->output, s->noutput);
    put(&b, ", \"tail\": ");
    json_list(&b, s, s->tail, s->ntail);
    put(&b, ", \"launcher\": ");
    if (s->launcher_build) {
        put(&b, "{");
        json_field(&b, s, "build", s->launcher_build, 1);
        json_field(&b, s, "message", s->launcher_message, 0);
        put(&b, "}");
    } else
        put(&b, "null");
    put(&b, "}");
    return take(&b);
}

/* ============================================================ markdown */
/* A gap in the summary: what goes before its text and after it, by kind */
static const char* const gap_before[] = {"shader not translated: ",
                                         "compute shader not translated: ",
                                         "methods of class ",
                                         "",
                                         "macro with no native form: ",
                                         "macro not compiled: ",
                                         "",
                                         "",
                                         ""};
static const char* const gap_after[] = {"", "", ", which no engine runs", "", "", "", "", " not implemented", ""};

/* Markdown of what a log says, which anyone can write: a summary a bot posts must not ping people, link, embed or close
   its own code blocks. GitHub links an issue reference (#N, GH-N, owner/repo#N), a URL or www. even escaped, but
   nothing in a code span: every value from the log is a code span, or a line of a fenced block */
static void md_code(Buf* b, const char* text) {
    put(b, "`");
    for (; *text; text++) put_n(b, *text == '`' ? "'" : text, 1);
    put(b, "`");
}
static void md_line(Buf* b, const char* text) {
    size_t i;
    for (i = 0; text[i]; i++)
        if (text[i] == '`' && text[i + 1] == '`' && text[i + 2] == '`') { /* no fence closed from inside */
            put(b, "'");
            while (text[i + 1] == '`') i++;
        } else
            put_n(b, text + i, 1);
    put(b, "\n");
}
static void md_sym(Buf* b, const SnxLogScan* s, const SnxLogSym* sym, SnxDemangle demangle) {
    const char* name = snx_logscan_text(s, sym->name);
    char* plain = demangle && !strncmp(name, "_Z", 2) ? demangle(name) : NULL;
    put(b, plain ? plain : name);
    if (sym->offset) {
        put(b, "+");
        put(b, snx_logscan_text(s, sym->offset));
    }
    free(plain);
}
static void md_fold_start(Buf* b, const char* summary) {
    put(b, "\n<details><summary>");
    put(b, summary);
    put(b, "</summary>\n\n```\n");
}
static void md_fold_end(Buf* b) { put(b, "```\n</details>\n"); }

char* snx_logscan_markdown(const SnxLogScan* s, const char* name, const char* heading, int brief,
                           SnxDemangle demangle) {
    static const char* const ipc_kinds[] = {"bad request", "undeclared", "unknown", "unknown control"};
    Buf b = {0};
    char num[32];
    put(&b, heading);
    put(&b, " ");
    md_code(&b, s->title ? snx_logscan_text(s, s->title) : name);
    if (s->title_id) {
        put(&b, " (");
        md_code(&b, snx_logscan_text(s, s->title_id));
        put(&b, ", version ");
        md_code(&b, snx_logscan_text(s, s->game_version));
        put(&b, ")");
    }
    put(&b, "\n\n- **Build:** ");
    if (s->build)
        md_code(&b, snx_logscan_text(s, s->build));
    else
        put(&b, "not in the log");
    if (s->system) {
        put(&b, "\n- **System:** ");
        md_code(&b, snx_logscan_text(s, s->system));
    }
    if (s->gpu) {
        put(&b, "\n- **GPU:** ");
        md_code(&b, snx_logscan_text(s, s->gpu));
        put(&b, ", Vulkan ");
        md_code(&b, snx_logscan_text(s, s->vulkan));
        put(&b, ", driver ");
        md_code(&b, snx_logscan_text(s, s->driver));
    }
    if (s->launcher_message) {
        put(&b, "\n- **The launcher said:** ");
        md_code(&b, snx_logscan_text(s, s->launcher_message));
    }
    put(&b, "\n- **Ended:** ");
    put(&b, snx_logscan_outcome_name(s->outcome));
    if (s->centiseconds >= 0) {
        sprintf(num, " after %ld.%ld s", (s->centiseconds + 5) / 100, (s->centiseconds + 5) % 100 / 10);
        put(&b, num);
    }
    if (s->signature) {
        put(&b, "\n- **Signature:** ");
        md_code(&b, snx_logscan_text(s, s->signature));
    }
    if (s->outcome != SNX_LOG_NONE) {
        if (snx_logscan_function(s)) {
            Buf sym = {0};
            md_sym(&sym, s, snx_logscan_function(s), demangle);
            put(&b, "\n- **In:** ");
            md_code(&b, sym.p ? sym.p : "");
            free(sym.p);
        }
        if (s->fatal && s->outcome != SNX_LOG_FATAL) {
            put(&b, "\n- **Message:** ");
            md_code(&b, snx_logscan_text(s, s->fatal));
        }
        put(&b, "\n");
        if (s->ndetails) {
            put(&b, "\n```\n");
            for (int i = 0; i < s->ndetails; i++) md_line(&b, snx_logscan_text(s, s->details[i]));
            put(&b, "```\n");
        }
        if (s->nframes) {
            md_fold_start(&b, "Backtrace");
            for (int i = 0; i < s->nframes; i++) {
                Buf sym = {0};
                put(&sym, "#");
                put_num(&sym, (unsigned long)i);
                put(&sym, " ");
                put(&sym, snx_logscan_text(s, s->frames[i].address));
                if (s->frames[i].sym.name) {
                    put(&sym, " ");
                    md_sym(&sym, s, &s->frames[i].sym, demangle);
                }
                md_line(&b, sym.p ? sym.p : "");
                free(sym.p);
            }
            md_fold_end(&b);
        }
    } else
        put(&b, "\n");
    if (s->nslow || s->frame_stats) {
        int order[SNX_LOGSCAN_SLOW];
        put(&b, "\n**Frames over 50 ms:** ");
        put_num(&b, s->slow_counted + s->slow_since);
        if (s->slow_counted + s->slow_since) {
            put(&b, ", the slowest ");
            put_num(&b, s->slowest_ms);
            put(&b, " ms");
        }
        put(&b, "\n");
        md_fold_start(&b, "What the slowest frames did, and the GPU's counts");
        slowest_first(s, order);
        for (int i = 0; i < s->nslow; i++) md_line(&b, snx_logscan_text(s, s->slow_line[order[i]]));
        if (s->frame_stats) md_line(&b, snx_logscan_text(s, s->frame_stats));
        if (s->memory) md_line(&b, snx_logscan_text(s, s->memory));
        md_fold_end(&b);
    }
    if (s->nipc) {
        put(&b, "\n**Service commands the runtime does not implement:**\n\n");
        for (int i = 0; i < s->nipc; i++) {
            put(&b, "- ");
            md_code(&b, snx_logscan_text(s, s->ipc[i].iface));
            put(&b, " cmd ");
            put_num(&b, s->ipc[i].command);
            put(&b, " (");
            put(&b, ipc_kinds[s->ipc[i].kind]);
            put(&b, ", ");
            put_num(&b, s->ipc[i].count);
            put(&b, "x)\n");
        }
    }
    if (s->ngaps || s->mme || s->mme_most || jit_missed(s)) {
        static const char* const areas[] = {"GPU", "Macros (MME)", "CPU"};
        put(&b, "\n**What else the runtime missed:**\n\n");
        for (int area = 0; area < 3; area++) {
            int any = (area == 1 && (s->mme || s->mme_most)) || (area == 2 && jit_missed(s));
            for (int i = 0; i < s->ngaps && !any; i++) any = gap_area(s->gaps[i].kind) == area;
            if (!any) continue;
            put(&b, "- **");
            put(&b, areas[area]);
            put(&b, ":**\n");
            if (area == 1 && s->mme) {
                put(&b, "  - macro calls: ");
                md_code(&b, snx_logscan_text(s, s->mme));
                put(&b, "\n");
            }
            if (area == 1 && s->mme_most) {
                put(&b, "  - interpreted most, by hash: ");
                md_code(&b, snx_logscan_text(s, s->mme_most));
                put(&b, "\n");
            }
            if (area == 2 && (s->jit & 1) && s->jit_earlier) {
                put(&b, "  - guest code the recompiler missed in earlier runs: ");
                put_num(&b, s->jit_earlier);
                put(&b, " pcs, queued for the JIT at startup\n");
            }
            if (area == 2 && (s->jit & 2) && (s->jit_compiled || s->jit_failed || s->jit_queued)) {
                put(&b, "  - guest code the recompiler missed, compiled at run time: ");
                put_num(&b, s->jit_compiled);
                put(&b, " pcs (");
                put_num(&b, s->jit_failed);
                put(&b, " failed, ");
                put_num(&b, s->jit_queued);
                put(&b, " still queued)\n");
            }
            for (int i = 0; i < s->ngaps; i++) {
                int kind = s->gaps[i].kind;
                if (gap_area(kind) != area) continue;
                put(&b, "  - ");
                put(&b, gap_before[kind]);
                md_code(&b, snx_logscan_text(s, s->gaps[i].what));
                put(&b, gap_after[kind]);
                if (s->gaps[i].count > 1) {
                    put(&b, " (");
                    put_num(&b, s->gaps[i].count);
                    put(&b, kind == SNX_GAP_SHADER || kind == SNX_GAP_COMPUTE ? " shaders)" : "x)");
                }
                put(&b, "\n");
            }
        }
    }
    if (s->nwarnings) {
        md_fold_start(&b, "Warnings");
        for (int i = 0; i < s->nwarnings; i++) {
            Buf sym = {0};
            put(&sym, snx_logscan_text(s, s->warnings[i].message));
            if (s->warnings[i].count > 1) {
                put(&sym, " (");
                put_num(&sym, s->warnings[i].count);
                put(&sym, "x)");
            }
            md_line(&b, sym.p ? sym.p : "");
            free(sym.p);
        }
        md_fold_end(&b);
    }
    if (s->noutput) {
        md_fold_start(&b, "The game's last output");
        for (int i = 0; i < s->noutput; i++) md_line(&b, snx_logscan_text(s, s->output[i]));
        md_fold_end(&b);
    }
    if (!brief && s->ntail) {
        md_fold_start(&b, "The log's last lines");
        for (int i = 0; i < s->ntail; i++) md_line(&b, snx_logscan_text(s, s->tail[i]));
        md_fold_end(&b);
    }
    return take(&b);
}

/* ============================================================ scarletnx --scan-log */
int snx_logscan_main(int argc, char** argv, SnxDemangle demangle) {
    int i, markdown = 0, files = 0, rc = 0;
    const char* name = NULL;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--name") && i + 1 < argc)
            name = argv[++i];
        else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
            if (strcmp(argv[i + 1], "json") && strcmp(argv[i + 1], "markdown")) {
                fprintf(stderr, "%s: --format is json or markdown\n", argv[0]);
                return 2;
            }
            markdown = !strcmp(argv[++i], "markdown");
        } else
            files++;
    if (!files) {
        fprintf(stderr, "usage: %s LOG... [--format json|markdown] [--name NAME]   (- reads stdin)\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        char* out;
        SnxLogScan s;
        if (!strcmp(argv[i], "--format") || !strcmp(argv[i], "--name")) {
            i++;
            continue;
        }
        FILE* f = strcmp(argv[i], "-") ? fopen(argv[i], "rb") : stdin;
        if (!f) {
            fprintf(stderr, "%s: %s cannot be read\n", argv[0], argv[i]);
            rc = 1;
            continue;
        }
        if (snx_logscan_file(&s, f)) {
            fprintf(stderr, "%s: %s: out of memory or a read error\n", argv[0], argv[i]);
            snx_logscan_free(&s);
            if (f != stdin) fclose(f);
            rc = 1;
            continue;
        }
        if (f != stdin) fclose(f);
        out = markdown ? snx_logscan_markdown(&s, name ? name : argv[i], "###", 0, demangle)
                       : snx_logscan_json(&s, demangle);
        if (out) printf("%s\n", out);
        free(out);
        snx_logscan_free(&s);
    }
    return rc;
}
