/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hookgen: generates the runtime's hooks from their declarations (src/nx/hle/hook/hooks.hook). A hook replaces a
   function the game exports with a handler of the runtime's, found by the function's symbol as the game's dynamic
   symbol table spells it: a C++ function by its mangled name (_ZN2nn2os...), never parsed or demangled. A
   declaration gives the function's arguments and result as C sees them (a C++ method's this is its first argument),
   and the handler that does its work:

     # a comment
     family libc
     hook memcpy(addr dst, addr src, u64 size) -> addr = hook_memmove

   A family names the hooks after it, up to the next family line, so the hooks_off setting can turn them off
   together; each hook belongs to one.

   The types are void (a result only), bool, u8, u16, u32, u64, i8, i16, i32, i64, addr (a guest address, u64), f32
   and f64. Under AAPCS64 an integer, bool or address takes the next of x0-x7 and a float the next of v0-v7, so
   nine of one kind would need the stack, which no hook takes. The result goes back in x0 or v0.

   It writes two files. The header (--header) declares each handler, `ret handler(CPU* c, args...)` (bool as int,
   addr as u64), for the files that define them. The table (--out, hle/hook/hook.c includes it) has an adapter for
   each hook, a GuestFn that reads the arguments from the CPU, calls the handler, puts its result in x0 or v0 and
   returns to x30 as the function's ret would, the hooks sorted by symbol (hook_symbol, hook_fn, hook_handler and
   hook_family, an index into hook_family_name), for a binary search by name. It is strict C99 and depends on nothing, so a cross build can build it for the
   machine it runs on (cmake/hookgen.cmake).

   Usage: hookgen --header OUT.h --out OUT.inc SPEC
   Each output is only rewritten when it changes. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HOOKS 512
#define MAX_FAMILIES 64
#define MAX_ARGS  16
#define MAX_NAME  256
#define NREGS     8

typedef enum { T_VOID, T_BOOL, T_U8, T_U16, T_U32, T_U64, T_I8, T_I16, T_I32, T_I64, T_ADDR, T_F32, T_F64 } Kind;

static const struct {
    const char* name;  /* in the declarations */
    const char* ctype; /* the handler's */
} kinds[] = {{"void", "void"}, {"bool", "int"}, {"u8", "u8"},   {"u16", "u16"},   {"u32", "u32"},
             {"u64", "u64"},   {"i8", "i8"},    {"i16", "i16"}, {"i32", "i32"},   {"i64", "i64"},
             {"addr", "u64"},  {"f32", "f32"},  {"f64", "f64"}};

typedef struct {
    Kind kind;
    char name[MAX_NAME];
    int reg; /* its x or v register */
} Arg;

typedef struct {
    char symbol[MAX_NAME], handler[MAX_NAME];
    Arg args[MAX_ARGS];
    int nargs, line, family;
    Kind ret;
} Hook;

static Hook hooks[MAX_HOOKS];
static int nhooks;
static char families[MAX_FAMILIES][MAX_NAME];
static int nfamilies;
static const char* spec_path;

static char* obuf;
static size_t olen, ocap;

static void fail(int line, const char* fmt, ...) {
    va_list ap;
    if (line)
        fprintf(stderr, "%s:%d: ", spec_path, line);
    else
        fprintf(stderr, "hookgen: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void out(const char* fmt, ...) {
    char line[1024];
    size_t n;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
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

/* written only when it changes, so its includers recompile only then (Ninja's restat) */
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

/* ============================================================ the declarations */
static const char* skip_space(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}
/* a symbol or identifier into buf (letters, digits, _, and the . and $ a symbol may hold): the text after it */
static const char* word(const char* p, char* buf, int line, const char* what) {
    size_t n = 0;
    p = skip_space(p);
    while (isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '$') {
        if (n + 1 >= MAX_NAME) fail(line, "the %s is too long", what);
        buf[n++] = *p++;
    }
    buf[n] = 0;
    if (!n) fail(line, "expected the %s", what);
    return p;
}
static const char* expect(const char* p, const char* token, int line) {
    size_t n = strlen(token);
    p = skip_space(p);
    if (strncmp(p, token, n)) fail(line, "expected '%s'", token);
    return p + n;
}
static Kind find_kind(const char* name, int line) {
    size_t i;
    for (i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (!strcmp(kinds[i].name, name)) return (Kind)i;
    fail(line, "unknown type '%s'", name);
    return T_VOID;
}
static int is_float(Kind k) { return k == T_F32 || k == T_F64; }
static int is_identifier(const char* s) {
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}

/* hook SYMBOL(TYPE NAME, ...) [-> TYPE] = HANDLER */
static void parse_hook(const char* p, int line) {
    Hook* h;
    char type[MAX_NAME];
    int x = 0, v = 0, i;
    if (nhooks == MAX_HOOKS) fail(line, "more than %d hooks", MAX_HOOKS);
    if (!nfamilies) fail(line, "a hook comes after a family line, which names its family");
    h = &hooks[nhooks++];
    memset(h, 0, sizeof *h);
    h->line = line;
    h->family = nfamilies - 1;
    p = word(p, h->symbol, line, "symbol");
    p = expect(p, "(", line);
    p = skip_space(p);
    while (*p != ')') {
        Arg* a;
        if (h->nargs == MAX_ARGS) fail(line, "more than %d arguments", MAX_ARGS);
        a = &h->args[h->nargs++];
        p = word(p, type, line, "argument's type");
        a->kind = find_kind(type, line);
        if (a->kind == T_VOID) fail(line, "an argument can't be void");
        p = word(p, a->name, line, "argument's name");
        if (!is_identifier(a->name) || !strcmp(a->name, "c")) fail(line, "'%s' can't name an argument", a->name);
        a->reg = is_float(a->kind) ? v++ : x++;
        if (x > NREGS || v > NREGS) fail(line, "an argument past x7 or v7 would be on the stack, which no hook reads");
        p = skip_space(p);
        if (*p == ',')
            p = skip_space(p + 1);
        else if (*p != ')')
            fail(line, "expected ',' or ')'");
    }
    p = skip_space(p + 1);
    h->ret = T_VOID;
    if (!strncmp(p, "->", 2)) {
        p = word(p + 2, type, line, "result's type");
        h->ret = find_kind(type, line);
    }
    p = expect(p, "=", line);
    p = word(p, h->handler, line, "handler");
    if (!is_identifier(h->handler)) fail(line, "'%s' can't name a handler", h->handler);
    if (*skip_space(p)) fail(line, "unexpected text after the handler");
    for (i = 0; i < nhooks - 1; i++)
        if (!strcmp(hooks[i].symbol, h->symbol)) fail(line, "%s is hooked on line %d too", h->symbol, hooks[i].line);
}

/* family NAME: the hooks after it, until the next */
static void parse_family(const char* p, int line) {
    int i;
    if (nfamilies == MAX_FAMILIES) fail(line, "more than %d families", MAX_FAMILIES);
    p = word(p, families[nfamilies], line, "family");
    if (!is_identifier(families[nfamilies])) fail(line, "'%s' can't name a family", families[nfamilies]);
    if (*skip_space(p)) fail(line, "unexpected text after the family");
    for (i = 0; i < nfamilies; i++)
        if (!strcmp(families[i], families[nfamilies])) fail(line, "family %s is named twice", families[i]);
    nfamilies++;
}

static void read_spec(const char* path) {
    char buf[2048];
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
        if (!strncmp(p, "hook ", 5) || !strncmp(p, "hook\t", 5))
            parse_hook(p + 5, line);
        else if (!strncmp(p, "family ", 7) || !strncmp(p, "family\t", 7))
            parse_family(p + 7, line);
        else
            fail(line, "expected a family, a hook or a comment");
    }
    fclose(f);
}

/* ============================================================ the output */
static int by_symbol(const void* a, const void* b) {
    return strcmp(((const Hook*)a)->symbol, ((const Hook*)b)->symbol);
}

/* two handlers of one name must take the same arguments: the header declares it once */
static void check_handlers(void) {
    int i, j, k;
    for (i = 0; i < nhooks; i++)
        for (j = 0; j < i; j++) {
            const Hook *a = &hooks[i], *b = &hooks[j];
            int same = a->ret == b->ret && a->nargs == b->nargs;
            if (strcmp(a->handler, b->handler)) continue;
            for (k = 0; same && k < a->nargs; k++)
                same = !strcmp(kinds[a->args[k].kind].ctype, kinds[b->args[k].kind].ctype);
            if (!same) fail(a->line, "%s takes other arguments on line %d", a->handler, b->line);
        }
}

static void prototype(const Hook* h) {
    int i;
    out("%s %s(CPU* c", kinds[h->ret].ctype, h->handler);
    for (i = 0; i < h->nargs; i++) out(", %s %s", kinds[h->args[i].kind].ctype, h->args[i].name);
    out(");\n");
}

/* an argument as the handler takes it, from its register */
static void argument(const Arg* a) {
    switch (a->kind) {
        case T_BOOL: out("(c->x[%d] & 0xFF) != 0", a->reg); break;
        case T_U64:
        case T_ADDR: out("c->x[%d]", a->reg); break;
        case T_F32: out("hook_f32(c, %d)", a->reg); break;
        case T_F64: out("hook_f64(c, %d)", a->reg); break;
        default: out("(%s)c->x[%d]", kinds[a->kind].ctype, a->reg); break;
    }
}

/* the adapter: a GuestFn that calls the handler and returns as the function's ret */
static void adapter(const Hook* h, int index) {
    int i;
    out("/* %s */\n", h->symbol);
    out("static void hook_fn_%d(CPU* RESTRICT c, u64 entry) {\n", index);
    out("    (void)entry;\n");
    out("    ");
    switch (h->ret) {
        case T_VOID: break;
        case T_F32: out("hook_set_f32(c, "); break;
        case T_F64: out("hook_set_f64(c, "); break;
        /* a result narrower than 32 bits extended to 32, as a C caller compiled for the guest may read all of w0 */
        case T_BOOL:
        case T_U8:
        case T_U16:
        case T_U32: out("c->x[0] = (u32)("); break;
        case T_I8:
        case T_I16:
        case T_I32: out("c->x[0] = (u32)(i32)("); break;
        default: out("c->x[0] = (u64)("); break;
    }
    out("%s(c", h->handler);
    for (i = 0; i < h->nargs; i++) {
        out(", ");
        argument(&h->args[i]);
    }
    out(h->ret == T_VOID ? ");\n" : "));\n");
    out("    c->pc = c->x[30];\n");
    out("}\n");
}

static void write_header(const char* path) {
    int i, j;
    out("/* Generated by hookgen from %s: the hooks' handlers. Don't edit */\n", spec_path);
    out("#ifndef SNX_HOOKS_HOOK_H\n#define SNX_HOOKS_HOOK_H\n");
    out("#include \"common/cpu/aarch64/cpu.h\"\n\n");
    for (i = 0; i < nhooks; i++) {
        for (j = 0; j < i && strcmp(hooks[j].handler, hooks[i].handler); j++) {
        }
        if (j == i) prototype(&hooks[i]);
    }
    out("#endif\n");
    flush(path);
}

static void write_table(const char* path) {
    int i;
    out("/* Generated by hookgen from %s: the hooks' adapters and their table, sorted by symbol. Don't edit */\n",
        spec_path);
    out("#define HOOK_COUNT %d\n#define HOOK_FAMILIES %d\n\n", nhooks, nfamilies);
    for (i = 0; i < nhooks; i++) adapter(&hooks[i], i);
    out("\nstatic const char* const hook_symbol[HOOK_COUNT + 1] = {\n");
    for (i = 0; i < nhooks; i++) out("    \"%s\",\n", hooks[i].symbol);
    out("    0};\nstatic const char* const hook_handler[HOOK_COUNT + 1] = {\n");
    for (i = 0; i < nhooks; i++) out("    \"%s\",\n", hooks[i].handler);
    out("    0};\nstatic const GuestFn hook_fn[HOOK_COUNT + 1] = {\n");
    for (i = 0; i < nhooks; i++) out("    hook_fn_%d,\n", i);
    out("    0};\nstatic const unsigned char hook_family[HOOK_COUNT + 1] = {\n");
    for (i = 0; i < nhooks; i++) out("    %d,\n", hooks[i].family);
    out("    0};\nstatic const char* const hook_family_name[HOOK_FAMILIES + 1] = {\n");
    for (i = 0; i < nfamilies; i++) out("    \"%s\",\n", families[i]);
    out("    0};\n");
    flush(path);
}

int main(int argc, char** argv) {
    const char *header = NULL, *table = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--header") && i + 1 < argc)
            header = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            table = argv[++i];
        else if (argv[i][0] != '-' && !spec_path)
            spec_path = argv[i];
        else
            spec_path = NULL, i = argc;
    }
    if (!spec_path || (!header && !table)) {
        fprintf(stderr, "usage: hookgen --header OUT.h --out OUT.inc SPEC\n");
        return 2;
    }
    read_spec(spec_path);
    qsort(hooks, (size_t)nhooks, sizeof *hooks, by_symbol);
    check_handlers();
    if (header) write_header(header);
    if (table) write_table(table);
    free(obuf);
    return 0;
}
