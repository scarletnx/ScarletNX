/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (tools/svc_generator.py: the declarations' parsing and the register allocation):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project */
/* svcgen: generates the SVC dispatch from the kernel's SVC declarations (src/nx/hle/svc/hos.svc). Each SVC's
   registers follow from its declaration as Eden's generator lays them out, for the 64-bit ABI (AAPCS64: eight 8-byte
   registers, all of them parameters) and the 32-bit one (AAPCS32: eight 4-byte registers, four of them parameters,
   the rest taken from the first free one):
   - an output passed by pointer (out or out_* with a *) has no input register but takes the parameter's place in the
     count. Outputs go out in their own count after the return value, unaligned
   - an input wider than a register takes several, starting at an even one while they are parameters
   - an output without a * (out_* by value) is an address the SVC writes through, so it is an input
   It writes one file, included by svc.c, with the handlers' prototypes (svc_<Name>, the declaration's arguments in
   order: outputs as pointers, an Address or Size as u64 in both ABIs), a wrapper for each ABI (svc64_<Name> from a
   CPU's x registers, svc32_<Name> from a SnxArmCPU's r registers, both through svc.c's svc_begin and svc_end), and
   the tables by number: g_svc64 and g_svc32 (svc64_unknown and svc32_unknown for every number without a handler,
   a stub's too) and g_svc_name. It is strict C99 and depends on nothing, so a cross build can build it for the
   machine it runs on.

   Usage: svcgen [--out OUT.inc] [--layout] SPEC
   --layout prints each SVC's registers as "inputs>outputs", each x:r, where x is the 64-bit ABI's register and r the
   32-bit one's (lo+hi for a value in two), for a check against Switchbrew's SVC page.
   The output is only rewritten when it changes. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TYPES 64
#define MAX_SVCS  256 /* SNX_SVC_COUNT (common/runtime/interface.h): numbers 0 to 0xFF */
#define MAX_ARGS  8
#define MAX_NAME  64
#define NREGS     8

/* the ABIs, Eden's BIT_32 and BIT_64 */
enum { A32, A64 };

typedef struct {
    char name[MAX_NAME];
    int size[2];         /* bytes in the 32-bit and the 64-bit ABI */
    char ctype[MAX_NAME]; /* what the handler takes, "-" for a structure no handler takes yet */
} Type;

typedef struct {
    int type;
    char name[MAX_NAME];
    int output, outptr;
    int regs[2][NREGS], nregs[2];
} Arg;

enum { F_STUB = 1, F_QUIET = 2, F_NOWARN = 4 };

typedef struct {
    int defined, line, flags, ret;
    char name[MAX_NAME];
    Arg args[MAX_ARGS];
    int nargs;
    int ret_regs[2][NREGS], nret_regs[2];
} Svc;

static Type types[MAX_TYPES];
static int ntypes;
static Svc svcs[MAX_SVCS];
static const char* spec_path;

static char* obuf;
static size_t olen, ocap;

static void fail(int line, const char* fmt, ...) {
    va_list ap;
    if (line)
        fprintf(stderr, "%s:%d: ", spec_path, line);
    else
        fprintf(stderr, "svcgen: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void out(const char* fmt, ...) {
    char line[512];
    size_t n;
    va_list ap;
    va_start(ap, fmt);
    vsprintf(line, fmt, ap);
    va_end(ap);
    n = strlen(line);
    if (olen + n + 1 > ocap) {
        ocap = (olen + n + 1) * 2;
        obuf = (char*)realloc(obuf, ocap);
        if (!obuf) fail(0, "out of memory");
    }
    memcpy(obuf + olen, line, n + 1);
    olen += n;
}

/* written only when it changes, so svc.c recompiles only then (Ninja's restat) */
static void flush(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        char* old = (char*)malloc(olen + 1);
        size_t got = old ? fread(old, 1, olen + 1, f) : 0;
        int same = old && got == olen && !memcmp(old, obuf, olen);
        free(old);
        fclose(f);
        if (same) {
            olen = 0;
            return;
        }
    }
    f = fopen(path, "wb");
    if (!f || fwrite(obuf, 1, olen, f) != olen || fclose(f)) fail(0, "cannot write %s", path);
    olen = 0;
}

/* ============================================================ the spec */
static const char* skip_space(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}
/* an identifier (letters, digits, _ and ::) into buf: the text after it */
static const char* word(const char* p, char* buf, int line, const char* what) {
    size_t n = 0;
    p = skip_space(p);
    while (isalnum((unsigned char)*p) || *p == '_' || *p == ':') {
        if (n + 1 >= MAX_NAME) fail(line, "the %s is too long", what);
        buf[n++] = *p++;
    }
    buf[n] = 0;
    if (!n) fail(line, "expected the %s", what);
    return p;
}
static const char* number(const char* p, int* v, int line, const char* what) {
    char* end;
    long n = strtol(skip_space(p), &end, 0);
    if (end == skip_space(p) || n < 0 || n > 0xFFFF) fail(line, "expected the %s", what);
    *v = (int)n;
    return end;
}
static int find_type(const char* name, int line) {
    int i;
    for (i = 0; i < ntypes; i++)
        if (!strcmp(types[i].name, name)) return i;
    fail(line, "unknown type '%s'", name);
    return -1;
}

/* type NAME SIZE32 SIZE64 CTYPE */
static void parse_type(const char* p, int line) {
    Type* t;
    if (ntypes == MAX_TYPES) fail(line, "too many types");
    t = &types[ntypes];
    p = word(p, t->name, line, "type's name");
    p = number(p, &t->size[A32], line, "type's 32-bit size");
    p = number(p, &t->size[A64], line, "type's 64-bit size");
    p = skip_space(p);
    if (*p == '-') {
        strcpy(t->ctype, "-");
        p++;
    } else
        p = word(p, t->ctype, line, "type's C type");
    if (*skip_space(p)) fail(line, "unexpected text after the type");
    if (t->size[A32] > 8 * 4 || t->size[A64] > 8 * 8) fail(line, "a type larger than the eight registers");
    for (t = types; t < &types[ntypes]; t++)
        if (!strcmp(t->name, types[ntypes].name)) fail(line, "type %s defined twice", t->name);
    ntypes++;
}

/* NUMBER [stub|quiet|nowarn]... RET NAME(TYPE[*] NAME, ...); with Eden's declarations as written */
static void parse_svc(const char* p, int line) {
    char w[MAX_NAME];
    int id, *flags;
    Svc* s;
    p = number(p, &id, line, "SVC's number");
    if (id >= MAX_SVCS) fail(line, "SVC 0x%x is past the table's 0x%x", id, MAX_SVCS);
    s = &svcs[id];
    if (s->defined) fail(line, "SVC 0x%02x declared again (line %d)", id, s->line);
    s->defined = 1;
    s->line = line;
    flags = &s->flags;
    for (;;) {
        p = word(p, w, line, "return type");
        if (!strcmp(w, "stub"))
            *flags |= F_STUB;
        else if (!strcmp(w, "quiet"))
            *flags |= F_QUIET;
        else if (!strcmp(w, "nowarn"))
            *flags |= F_NOWARN;
        else
            break;
    }
    s->ret = find_type(w, line);
    if (strcmp(types[s->ret].name, "Result")) *flags |= F_NOWARN; /* no result for the warning to look at */
    p = word(p, s->name, line, "SVC's name");
    p = skip_space(p);
    if (*p++ != '(') fail(line, "expected ( after the name");
    p = skip_space(p);
    while (*p != ')') {
        Arg* a;
        int star = 0;
        if (s->nargs == MAX_ARGS) fail(line, "more than %d arguments", MAX_ARGS);
        a = &s->args[s->nargs++];
        p = word(p, w, line, "argument's type");
        a->type = find_type(w, line);
        p = skip_space(p);
        if (*p == '*') {
            star = 1;
            p++;
        }
        p = word(p, a->name, line, "argument's name");
        /* Eden: every output is named out or out_*; one not passed by pointer is a user pointer, an input */
        a->output = !strcmp(a->name, "out") || strstr(a->name, "out_") != NULL;
        a->outptr = a->output && !star;
        if (star && !a->output) fail(line, "%s is passed by pointer, but isn't named as an output", a->name);
        p = skip_space(p);
        if (*p == ',')
            p = skip_space(p + 1);
        else if (*p != ')')
            fail(line, "expected , or ) after %s", a->name);
    }
    p = skip_space(p + 1);
    if (*p != ';' || *skip_space(p + 1)) fail(line, "expected ; at the end of the declaration");
}

static void read_spec(const char* path) {
    char buf[512];
    int line = 0;
    FILE* f = fopen(path, "r");
    if (!f) fail(0, "cannot open %s", path);
    while (fgets(buf, sizeof buf, f)) {
        const char* p = skip_space(buf);
        size_t n = strlen(buf);
        line++;
        if (n && buf[n - 1] != '\n' && !feof(f)) fail(line, "line too long");
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (!*p || *p == '#') continue;
        if (!strncmp(p, "type ", 5))
            parse_type(p + 5, line);
        else if (isdigit((unsigned char)*p))
            parse_svc(p, line);
        else
            fail(line, "expected a type, an SVC or a comment");
    }
    fclose(f);
}

/* ============================================================ registers (Eden's RegisterAllocator) */
typedef struct {
    int used[NREGS];
    int bytes, params, line;
} Alloc;

static int take(Alloc* al, int r) {
    if (al->used[r]) fail(al->line, "register %d allocated twice", r);
    al->used[r] = 1;
    return r;
}
/* one register at ngrn, or the first free one once ngrn is past the parameters: the register, and *inc the step */
static int add_single(Alloc* al, int ngrn, int* inc) {
    int i;
    if (ngrn < al->params) {
        *inc = 1;
        return take(al, ngrn);
    }
    *inc = 0;
    for (i = 0; i < NREGS; i++)
        if (!al->used[i]) return take(al, i);
    fail(al->line, "no registers left");
    return -1;
}
/* the registers of a value of size bytes at ngrn: their count in regs, the step in *inc */
static int add(Alloc* al, int ngrn, int size, int align, int* regs, int* inc) {
    int n = 0, i, left = size;
    if (size <= al->bytes) {
        regs[0] = add_single(al, ngrn, inc);
        return 1;
    }
    *inc = align ? ngrn % 2 : 0;
    while (left > 0) {
        regs[n++] = add_single(al, ngrn + *inc, &i);
        *inc += i;
        left -= al->bytes;
    }
    return n;
}
static void layout(Svc* s, int abi) {
    Alloc in, outs;
    int i, inc, ngrn = 0;
    memset(&in, 0, sizeof in);
    in.bytes = abi == A64 ? 8 : 4;
    in.params = abi == A64 ? 8 : 4;
    in.line = s->line;
    outs = in;
    for (i = 0; i < s->nargs; i++) {
        Arg* a = &s->args[i];
        if (a->output && !a->outptr) {
            ngrn++;
            continue;
        }
        a->nregs[abi] = add(&in, ngrn, types[a->type].size[abi], 1, a->regs[abi], &inc);
        ngrn += inc;
    }
    ngrn = 0;
    if (types[s->ret].size[abi]) {
        s->nret_regs[abi] = add(&outs, ngrn, types[s->ret].size[abi], 0, s->ret_regs[abi], &inc);
        ngrn += inc;
    }
    for (i = 0; i < s->nargs; i++) {
        Arg* a = &s->args[i];
        if (!a->output || a->outptr) continue;
        a->nregs[abi] = add(&outs, ngrn, types[a->type].size[abi], 0, a->regs[abi], &inc);
        ngrn += inc;
    }
}

/* ============================================================ the output */
static int is_output(const Arg* a) { return a->output && !a->outptr; }
static const char* ctype(int t) { return types[t].ctype; }
static const char* utype(int size) { return size <= 1 ? "u8" : size <= 2 ? "u16" : size <= 4 ? "u32" : "u64"; }

static void check_handler(const Svc* s) {
    int i;
    if (!strcmp(ctype(s->ret), "-")) fail(s->line, "%s returns a structure no handler takes", s->name);
    for (i = 0; i < s->nargs; i++) {
        const Arg* a = &s->args[i];
        if (!strcmp(ctype(a->type), "-")) fail(s->line, "%s: %s is a structure no handler takes yet", s->name, a->name);
        if (types[a->type].size[A64] > 8) fail(s->line, "%s: %s is wider than a register", s->name, a->name);
    }
}

static void prototype(const Svc* s) {
    int i;
    out("static %s svc_%s(", ctype(s->ret), s->name);
    for (i = 0; i < s->nargs; i++) {
        const Arg* a = &s->args[i];
        out("%s%s%s %s", i ? ", " : "", ctype(a->type), is_output(a) ? "*" : "", a->name);
    }
    out("%s);\n", s->nargs ? "" : "void");
}

/* a register's value as the handler's type: raw is a u64 or a u32 expression */
static void convert(const char* type, const char* raw) {
    if (!strcmp(type, "i32"))
        out("(i32)(u32)%s", raw);
    else
        out("(%s)%s", type, raw);
}

static void input(const Svc* s, const Arg* a, int abi) {
    char raw[64];
    const int* r = a->regs[abi];
    if (abi == A64)
        sprintf(raw, "c->x[%d]", r[0]);
    else if (a->nregs[A32] == 1)
        sprintf(raw, "a->r[%d]", r[0]);
    else if (a->nregs[A32] == 2)
        sprintf(raw, "((u64)a->r[%d] | (u64)a->r[%d] << 32)", r[0], r[1]);
    else
        fail(s->line, "%s: %s takes %d registers", s->name, a->name, a->nregs[A32]);
    convert(ctype(a->type), raw);
}

/* value (of size bytes in this ABI) into its registers */
static void scatter(const char* value, int size, const int* regs, int n, int abi) {
    int i;
    if (abi == A64) {
        out("    c->x[%d] = (%s)%s;\n", regs[0], utype(size), value);
        return;
    }
    for (i = 0; i < n; i++)
        if (i)
            out("    a->r[%d] = (u32)((u64)%s >> %d);\n", regs[i], value, 32 * i);
        else
            out("    a->r[%d] = (%s)%s;\n", regs[i], utype(size < 4 ? size : 4), value);
}

static const char* flag_names(int flags) {
    switch (flags & (F_QUIET | F_NOWARN)) {
        case F_QUIET: return "SVC_QUIET";
        case F_NOWARN: return "SVC_NOWARN";
        case F_QUIET | F_NOWARN: return "SVC_QUIET | SVC_NOWARN";
        default: return "0";
    }
}

/* Handlers that don't return: a zone begun before one would never end. Break returns for its high bit. */
static int svc_returns(const Svc* s) {
    return strcmp(s->name, "ExitThread") && strcmp(s->name, "ExitProcess") && strcmp(s->name, "Break") &&
           strcmp(s->name, "ReturnFromException");
}
/* Waits can last the rest of the run, so they get no zone. */
static int svc_zones(const Svc* s) {
    static const char* const blocking[] = {"SleepThread", "WaitSynchronization", "ArbitrateLock",
                                           "WaitProcessWideKeyAtomic", "WaitForAddress"};
    size_t i;
    if (!svc_returns(s)) return 0;
    for (i = 0; i < sizeof blocking / sizeof blocking[0]; i++)
        if (!strcmp(s->name, blocking[i])) return 0;
    return 1;
}

static void wrapper(const Svc* s, int abi) {
    const char* w = abi == A64 ? "x" : "r";
    int i, first = 1;
    if (abi == A64)
        out("static void svc64_%s(CPU* c, u32 imm) {\n    KThread* t = (KThread*)c->thread;\n", s->name);
    else
        out("static void svc32_%s(SnxArmCPU* a, u32 imm) {\n    KThread* t = kthread_cur();\n", s->name);
    out("    SvcRec in;\n");
    for (i = 0; i < s->nargs; i++)
        if (is_output(&s->args[i])) out("    %s %s = 0;\n", ctype(s->args[i].type), s->args[i].name);
    if (s->nret_regs[abi]) out("    %s ret;\n", ctype(s->ret));
    /* the zone spans the whole SVC, the register record and the handler both: its begin and end are the call's, so a
       frame's SVCs add up to the time it spent in them */
    if (svc_zones(s)) out("    SNX_ZONE(svc_zone, \"%s\");\n", s->name);
    out("    u32 n = svc_begin%d(t, &in, imm, %s, %s);\n", abi == A64 ? 64 : 32, abi == A64 ? "c" : "a",
        flag_names(s->flags));
    out("    %ssvc_%s(", s->nret_regs[abi] ? "ret = " : "", s->name);
    for (i = 0; i < s->nargs; i++) {
        const Arg* a = &s->args[i];
        if (!first) out(", ");
        first = 0;
        if (is_output(a))
            out("&%s", a->name);
        else
            input(s, a, abi);
    }
    out(");\n");
    if (s->nret_regs[abi]) scatter("ret", types[s->ret].size[abi], s->ret_regs[abi], s->nret_regs[abi], abi);
    for (i = 0; i < s->nargs; i++) {
        const Arg* a = &s->args[i];
        if (is_output(a)) scatter(a->name, types[a->type].size[abi], a->regs[abi], a->nregs[abi], abi);
    }
    /* the result on error only, after the outputs are written back: a horizon result is 32 bits and 0 is success, so
       the success path pays one compare. Its handlers already say what the error was; the trace's zone says it too */
    if (svc_zones(s))
        out("    if ((u32)%s->%s[0]) SNX_ZONE_VALUE(svc_zone, (u32)%s->%s[0]);\n", abi == A64 ? "c" : "a", w,
            abi == A64 ? "c" : "a", w);
    out("    svc_end(t, &in, n, %s, %s->%s[0], %s->%s[1]);\n", flag_names(s->flags), abi == A64 ? "c" : "a", w,
        abi == A64 ? "c" : "a", w);
    if (svc_zones(s)) out("    SNX_ZONE_END(svc_zone);\n");
    out("}\n");
}

static void table(const char* decl, const char* prefix, const char* unknown) {
    int id;
    out("%s = {\n", decl);
    for (id = 0; id < MAX_SVCS; id++) {
        const Svc* s = &svcs[id];
        if (s->defined && !(s->flags & F_STUB))
            out("    %s%s, /* %02x */\n", prefix, s->name, id);
        else
            out("    %s, /* %02x%s%s */\n", unknown, id, s->defined ? " " : "", s->defined ? s->name : "");
    }
    out("};\n");
}

static void write_inc(const char* path) {
    int id, abi;
    out("/* generated by svcgen from %s */\n", spec_path);
    out("#if SNX_SVC_COUNT != %d\n#error SNX_SVC_COUNT is not the size of svcgen's tables\n#endif\n\n", MAX_SVCS);
    out("static const char* const g_svc_name[SNX_SVC_COUNT] = {\n");
    for (id = 0; id < MAX_SVCS; id++)
        if (svcs[id].defined)
            out("    \"%s\",\n", svcs[id].name);
        else
            out("    NULL, /* %02x */\n", id);
    out("};\n\n");
    out("/* every number without a handler: svc.c */\n");
    out("static void svc64_unknown(CPU* c, u32 imm);\nstatic void svc32_unknown(SnxArmCPU* a, u32 imm);\n\n");
    out("/* the handlers svc.c defines */\n");
    for (id = 0; id < MAX_SVCS; id++)
        if (svcs[id].defined && !(svcs[id].flags & F_STUB)) {
            check_handler(&svcs[id]);
            prototype(&svcs[id]);
        }
    for (abi = A64; abi >= A32; abi--) {
        out("\n/* the %d-bit ABI's registers */\n", abi == A64 ? 64 : 32);
        for (id = 0; id < MAX_SVCS; id++)
            if (svcs[id].defined && !(svcs[id].flags & F_STUB)) wrapper(&svcs[id], abi);
    }
    out("\n");
    table("void (*const g_svc64[SNX_SVC_COUNT])(CPU* c, u32 imm)", "svc64_", "svc64_unknown");
    table("static void (*const g_svc32[SNX_SVC_COUNT])(SnxArmCPU* a, u32 imm)", "svc32_", "svc32_unknown");
    flush(path);
}

static void print_regs(const int* regs, int n) {
    int i;
    for (i = 0; i < n; i++) printf("%s%d", i ? "+" : "", regs[i]);
}
static void print_layout(void) {
    int id, i;
    for (id = 0; id < MAX_SVCS; id++) {
        const Svc* s = &svcs[id];
        int sep = 0;
        if (!s->defined) continue;
        printf("%02x ", id);
        for (i = 0; i < s->nargs; i++) {
            const Arg* a = &s->args[i];
            if (is_output(a)) continue;
            printf("%s", sep++ ? " " : "");
            print_regs(a->regs[A64], a->nregs[A64]);
            printf(":");
            print_regs(a->regs[A32], a->nregs[A32]);
        }
        printf(">");
        sep = 0;
        if (s->nret_regs[A64]) {
            sep = 1;
            print_regs(s->ret_regs[A64], s->nret_regs[A64]);
            printf(":");
            print_regs(s->ret_regs[A32], s->nret_regs[A32]);
        }
        for (i = 0; i < s->nargs; i++) {
            const Arg* a = &s->args[i];
            if (!is_output(a)) continue;
            printf("%s", sep++ ? " " : "");
            print_regs(a->regs[A64], a->nregs[A64]);
            printf(":");
            print_regs(a->regs[A32], a->nregs[A32]);
        }
        printf(" %s%s\n", s->name, s->flags & F_STUB ? " (stub)" : "");
    }
}

int main(int argc, char** argv) {
    const char* out_path = NULL;
    int i, show = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_path = argv[++i];
        else if (!strcmp(argv[i], "--layout"))
            show = 1;
        else if (argv[i][0] != '-' && !spec_path)
            spec_path = argv[i];
        else
            spec_path = NULL, i = argc;
    }
    if (!spec_path) {
        fprintf(stderr, "usage: svcgen [--out OUT.inc] [--layout] SPEC\n");
        return 2;
    }
    read_spec(spec_path);
    for (i = 0; i < MAX_SVCS; i++)
        if (svcs[i].defined) {
            layout(&svcs[i], A64);
            layout(&svcs[i], A32);
        }
    if (show) print_layout();
    if (out_path) write_inc(out_path);
    free(obuf);
    return 0;
}
