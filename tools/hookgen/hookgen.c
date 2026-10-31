/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hookgen: generates the runtime's hooks from their declarations (src/nx/hle/hook/hooks.hook). A hook replaces a
   function the game exports with a handler of the runtime's, found by the function's symbol as the game's dynamic
   symbol table spells it: a C++ function by its mangled name (_ZN2nn2os...), never parsed or demangled. A
   declaration gives the function's arguments and result as C sees them (a C++ method's this is its first argument),
   and the handler that does its work:

     # a comment
     family libc
     hook memcpy(void* dst, const void* src, u64 size) -> void* = hook_memmove
     hook nvnDeviceFinalize(NVNdevice* d)

   A family names the hooks after it, up to the next family line, so the hooks_off setting can turn them off
   together; each hook belongs to one. The handler's name follows '='. Without it, the handler is the symbol's name
   with a '_' before its first capital (nvnDeviceFinalize: nvn_DeviceFinalize), which the symbol must have after a
   part in lower case; an '= HANDLER' that names that handler is refused, as the symbol says it already.

   The types are void (a result only), bool, u8, u16, u32, u64, i8, i16, i32, i64, addr (a guest address, u64), f32
   and f64, each the size its name says in the guest (AArch64, LP64), and pointers: a type followed by '*', each '*'
   optionally followed by const, after an optional const (const NVNtexture* const* textures). A pointer points to
   void, char, one of the types above, or a type of the header that the spec's include line names (include
   "nx/gpu/nvn/nvn.h", found in the folders -I gives): hookgen reads the names its typedefs declare, and refuses an
   unknown one. A type of the header is passed by value only when it is an enum (an int under AAPCS64, as an i32)
   or a pointer (typedef void* NVNnativeWindow, or a function pointer, as an addr). The language has no arrays: an
   array argument is a pointer to its first element. Under AAPCS64 an integer, bool, pointer or address takes the
   next of x0-x7 and a float the next of v0-v7, so nine of one kind would need the stack, which no hook takes. The
   result goes back in x0 or v0.

   A pointer is a guest address: the adapter reads it from its x register as a u64 and the handler takes it as one,
   so no handler can mistake it for a host pointer. The pointer types say what the function's argument is, for the
   function types below and for the tools that read the spec (tools/hook_spec.py).

   Versions. A hook or a struct may hold only for some versions of what built the module that exports its function:

     @version(>= 12.0.0, < 16.0.0) hook _ZN2nn2os9LockMutexEPNS0_9MutexTypeE(addr m) = os_lock uses nn::os::MutexType

   or the @version(...) line alone before the hook's. It holds comparisons (>=, >, <=, <, ==) of one key's version,
   joined by commas, all of which hold. The key comes first and is sdk when there is none: sdk is the nnSdk version
   of the module's SDK label (x.y.z, each at most 255, packed x << 16 | y << 8 | z), nvn the NVN API version the
   runtime reads (major.minor, the minor at most 65535, packed major << 16 | minor). Missing parts are 0. A module
   whose version is unknown (0) runs only the unversioned hooks. One symbol may be declared several times with
   versions that don't overlap, each with its own handler, all of one key; an unversioned declaration is its symbol's
   only one.

   Structs. The layout of a struct of the guest's, which may change between versions:

     struct nn::os::MutexType @version(>= 9.0.0) {
         u32 state @0x0
         u32 owner @0x4
     } size 0x20

   Each field is a type (any but void, bool and the integers as wide as their names say, addr 8 bytes), a name and
   its offset, separated by line ends or semicolons, and the struct's size follows its brace; the one-line form is
   `struct S { u32 a @0; u32 b @4 } size 8`. A struct declared more than once declares the same fields (of the same
   types, at offsets of their own) in each, with versions that don't overlap. A hook whose handler reads one says so
   after the handler (`= handler uses nn::os::MutexType, ...`), and the handler takes a pointer to its layout after
   the CPU, the variant for the module the hook was installed in: hookgen splits the hook's versions at its variants'
   bounds into one entry each, so an entry's adapter passes its variant as a constant. The versions no variant covers
   have no entry, and the hook isn't installed for them.

   It writes two files. The header (--header) declares each struct's layout type, <Prefix>Layout_<name with :: as _>
   (its size, and each field as a HookField_<type> holding its offset, for hook.h's hook_get_<type> and
   hook_put_<type>), and each handler, `ret handler(CPU* c, layouts..., args...)` (bool as int, an address or a
   pointer as u64, an enum as i32), for the files that define them. A spec with an include line also gets each hooked
   function's type, as the guest calls it: PFN<SYMBOL in capitals>PROC, with the spec's C types (bool as uint8_t, an
   integer as the <stdint.h> type of its guest size, an addr as uint64_t, a pointer as its host pointer). Nothing
   calls through these types: a guest function runs at a guest address. They say what the arguments are, and a host
   pointer has the guest's 8 bytes (the header asserts it). All of one symbol's declarations take the same types.
   The table (--out, hle/hook/hook.c includes it) has each struct's variants, an adapter for each entry, a GuestFn
   that reads the arguments from the CPU, calls the handler, puts its result in x0 or v0 and returns to x30 as the
   function's ret would, and the entries sorted by symbol and then by version (hook_symbol, hook_fn, hook_handler,
   hook_family (an index into hook_family_name), hook_key, hook_lo and hook_hi (the versions [lo, hi) of key,
   hook.h's HOOK_ANY for unversioned), hook_layouts (the variants it passes, for the log) and hook_table, the view
   hook_select searches. It is strict C99 and depends on nothing, so a cross build can build it for the machine it
   runs on (cmake/hookgen.cmake).

   Verify mode (verify.h). A hook says `pure` after its handler (and its uses) when its function depends on nothing but
   its arguments, the memory they point to and constant data, and changes nothing but its result and the memory its
   buffer arguments name. A buffer is an addr or pointer argument followed by its use and its length in bytes:

     hook memcpy(void* dst out[size], const void* src in[size], u64 size) -> void* = hook_memmove pure

   in[...] is read, out[...] written and inout[...] both; the length is a number, or an integer argument's name
   optionally times a number, plus a number (in[count * 4 + 8]). A hook has at most 4 buffers, and only a pure one has
   any. The table's hook_verify gives each entry's: verify mode runs a pure hook's function on copies of its buffers
   and its handler on the live ones, and compares them.

   Routes. The recompiler calls a function through its dispatch slot, where a hook can take the call, only when a
   hook names the function's symbol (snx_codegen_routed): every other call stays a direct call. A function the runtime
   wraps with C code of its own, with no adapter, is named on a line of its own, `route SYMBOL`
   (gpu/nvn/nvn.hook's nvnBootstrapLoader).

   Usage: hookgen [--prefix NAME] [-I DIR]... --header OUT.h --out OUT.inc SPEC (NAME, hook by default, names the
          tables; each -I DIR a folder to find the include line's header in)
          hookgen [-I DIR]... --routes OUT.inc SPEC... (the symbols that the hooks and route lines of every SPEC
          name, sorted by strcmp and without duplicates, as hook_route_names and HOOK_ROUTE_COUNT, for
          nx/loader/hook_routes.c)
   Each output is only rewritten when it changes. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HOOKS    1024 /* NVN alone has about 540 functions */
#define MAX_ENTRIES  1024
#define MAX_FAMILIES 64
#define MAX_ARGS     16
#define MAX_USES     4
#define MAX_VARIANTS 256
#define MAX_FIELDS   64
#define MAX_NAME     256
#define MAX_ROUTES   64
#define MAX_SPECS    8
#define MAX_DIRS     8
#define MAX_TYPES    1024 /* the include line's header's typedefs: nvn.h has about 110 */
#define NREGS        8
#define UNBOUNDED    0xFFFFFFFFul

typedef enum { T_VOID, T_BOOL, T_U8, T_U16, T_U32, T_U64, T_I8, T_I16, T_I32, T_I64, T_ADDR, T_F32, T_F64 } Kind;

static const struct {
    const char* name;  /* in the declarations */
    const char* ctype; /* the handler's */
    const char* cname; /* the function type's: its size in the guest */
    unsigned bytes;    /* as a struct's field */
} kinds[] = {{"void", "void", "void", 0},   {"bool", "int", "uint8_t", 1},  {"u8", "u8", "uint8_t", 1},
             {"u16", "u16", "uint16_t", 2}, {"u32", "u32", "uint32_t", 4},  {"u64", "u64", "uint64_t", 8},
             {"i8", "i8", "int8_t", 1},     {"i16", "i16", "int16_t", 2},   {"i32", "i32", "int32_t", 4},
             {"i64", "i64", "int64_t", 8},  {"addr", "u64", "uint64_t", 8}, {"f32", "f32", "float", 4},
             {"f64", "f64", "double", 8}};

/* the version keys, as hook.h numbers them */
enum { K_ANY, K_SDK, K_NVN };
static const char* const key_name[] = {"any", "sdk", "nvn"};
static const char* const key_macro[] = {"HOOK_ANY", "HOOK_SDK", "HOOK_NVN"};
/* a result's kind for verify mode (verify.h), by Kind */
static const char* const ret_macro[] = {"HOOK_RET_VOID", "HOOK_RET_BOOL", "HOOK_RET_X8",   "HOOK_RET_X16",
                                        "HOOK_RET_X32",  "HOOK_RET_X64",  "HOOK_RET_X8",   "HOOK_RET_X16",
                                        "HOOK_RET_X32",  "HOOK_RET_X64",  "HOOK_RET_ADDR", "HOOK_RET_F32",
                                        "HOOK_RET_F64"};

/* the versions [lo, hi) of key; K_ANY (0 to UNBOUNDED) for none given */
typedef struct {
    int key;
    unsigned long lo, hi;
} Range;

/* how a buffer argument is used, as verify.h numbers them: 0 for an argument that isn't one */
enum { B_NONE, B_IN, B_OUT, B_INOUT };
static const char* const buf_mode[] = {"", "in", "out", "inout"};

typedef struct {
    Kind kind;
    char name[MAX_NAME];
    char cname[MAX_NAME]; /* its C type, in the function type: const NVNtexture* const* */
    int reg;              /* its x or v register */
    /* a buffer (an address with in[...], out[...] or inout[...]): its use and its length, len_add bytes and the
       argument len_name's value times len_mul, when there is one (len_arg its index once the arguments are read) */
    int mode, len_arg;
    char len_name[MAX_NAME];
    unsigned long len_mul, len_add;
} Arg;

typedef struct {
    char symbol[MAX_NAME], handler[MAX_NAME];
    Arg args[MAX_ARGS];
    int nargs, line, family, nuses, pure, nbufs;
    Kind ret;
    char ret_cname[MAX_NAME];
    Range range;
    char use_name[MAX_USES][MAX_NAME];
    int use[MAX_USES]; /* the structs' first variants */
} Hook;

typedef struct {
    Kind kind;
    char name[MAX_NAME];
    unsigned long offset;
} Field;

/* one declaration of a struct */
typedef struct {
    char name[MAX_NAME];  /* nn::os::MutexType */
    char cname[MAX_NAME]; /* nn_os_MutexType */
    Field fields[MAX_FIELDS];
    int nfields, line;
    int first; /* the struct's first variant, whose field order its layout type has */
    int index; /* in the struct's variants, in version order */
    unsigned long size;
    Range range;
} Variant;

/* a hook for one range of versions, with one variant of each struct it uses: the table's rows */
typedef struct {
    int hook;
    Range range;
    int variant[MAX_USES];
} Entry;

static Hook hooks[MAX_HOOKS];
static int nhooks;
static Variant variants[MAX_VARIANTS];
static int nvariants;
static Entry entries[MAX_ENTRIES];
static int nentries;
static char families[MAX_FAMILIES][MAX_NAME];
static int nfamilies;
static char routes[MAX_ROUTES][MAX_NAME]; /* the route lines' symbols */
static int route_lines[MAX_ROUTES];
static int nroutes;
static const char* spec_path;
/* the include line's header (the spec's at most one), and the names its typedefs declare */
enum { H_OTHER, H_ENUM, H_POINTER };
static char include_name[MAX_NAME];
static struct {
    char name[MAX_NAME];
    int what;
} types[MAX_TYPES];
static int ntypes;
static const char* include_dirs[MAX_DIRS];
static int ninclude_dirs;

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
/* whether p starts with keyword w, followed by a space, a tab or the end of the line */
static int keyword(const char* p, const char* w) {
    size_t n = strlen(w);
    return !strncmp(p, w, n) && (p[n] == ' ' || p[n] == '\t' || !p[n]);
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
/* a number, decimal or 0x hexadecimal, of at most 32 bits */
static const char* number(const char* p, unsigned long* v, int line, const char* what) {
    int hex, digits = 0;
    p = skip_space(p);
    hex = p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    if (hex) p += 2;
    *v = 0;
    while (hex ? isxdigit((unsigned char)*p) : isdigit((unsigned char)*p)) {
        unsigned long d =
            (unsigned long)(isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10);
        unsigned long base = hex ? 16 : 10;
        if (*v > (UNBOUNDED - d) / base) fail(line, "the %s is too large", what);
        *v = *v * base + d;
        p++, digits++;
    }
    if (!digits || isalnum((unsigned char)*p) || *p == '_') fail(line, "expected the %s, a number", what);
    return p;
}

/* ============================================================ the include line's header */
/* the next token of the C text at *pp: 1 for an identifier (into tok), a punctuation character as itself, 0 at the
   end. Spaces, comments, preprocessor lines, strings, characters and numbers are skipped */
static int c_token(const char** pp, char* tok, const char* path) {
    const char* p = *pp;
    int t;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (p[0] == '/' && p[1] == '*') {
            const char* end = strstr(p + 2, "*/");
            p = end ? end + 2 : p + strlen(p);
        } else if (p[0] == '/' && p[1] == '/') {
            p += strcspn(p, "\n");
        } else if (*p == '#') { /* to the end of the line, and the lines a backslash continues it on */
            while (*p && *p != '\n') p += p[0] == '\\' && p[1] ? 2 : 1;
        } else if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) p += p[0] == '\\' && p[1] ? 2 : 1;
            if (*p) p++;
        } else if (isdigit((unsigned char)*p)) {
            while (isalnum((unsigned char)*p) || *p == '_' || *p == '.') p++;
        } else {
            break;
        }
    }
    if (isalpha((unsigned char)*p) || *p == '_') {
        size_t n = 0;
        while (isalnum((unsigned char)*p) || *p == '_') {
            if (n + 1 >= MAX_NAME) fail(0, "%s: an identifier is too long", path);
            tok[n++] = *p++;
        }
        tok[n] = 0;
        t = 1;
    } else {
        t = (unsigned char)*p;
        if (*p) p++;
    }
    *pp = p;
    return t;
}

/* the names the typedefs of the include line's header declare, each an enum, a pointer (typedef void* P, or a
   function pointer, typedef R (*P)(...)) or another type, which only a pointer argument may point to */
static void read_header(int line) {
    char path[1024], tok[MAX_NAME];
    const char* p;
    char* text;
    long size = 0;
    FILE* f = NULL;
    int i, t;
    for (i = 0; !f && i < ninclude_dirs; i++) {
        snprintf(path, sizeof path, "%s/%s", include_dirs[i], include_name);
        f = fopen(path, "rb");
    }
    if (!f) fail(line, "cannot find %s: give the folder it is in with -I", include_name);
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) fail(line, "cannot read %s", path);
    text = (char*)malloc((size_t)size + 1);
    if (!text) fail(0, "out of memory");
    if (fread(text, 1, (size_t)size, f) != (size_t)size) fail(line, "cannot read %s", path);
    text[size] = 0;
    fclose(f);
    p = text;
    while ((t = c_token(&p, tok, path)) != 0) {
        char name[MAX_NAME] = {0}, last_name[MAX_NAME] = {0}, fn_name[MAX_NAME] = {0};
        int depth = 0, what = H_OTHER, star = 0, last[3] = {0, 0, 0};
        if (t != 1 || strcmp(tok, "typedef")) continue;
        t = c_token(&p, tok, path);
        if (t == 1 && !strcmp(tok, "enum")) what = H_ENUM;
        for (; t && !(t == ';' && !depth); t = c_token(&p, tok, path)) {
            if (t == ')' && depth == 1 && last[0] == '(' && last[1] == '*' && last[2] == 1) /* (*NAME) */
                strcpy(fn_name, last_name);
            if (t == '{' || t == '(' || t == '[') depth++;
            if (t == '}' || t == ')' || t == ']') depth--;
            if (t == '*' && !depth) star = 1;
            if (t == 1) {
                strcpy(last_name, tok);
                if (!depth) strcpy(name, tok);
            }
            last[0] = last[1], last[1] = last[2], last[2] = t;
        }
        if (fn_name[0]) strcpy(name, fn_name), star = 1;
        if (!name[0]) continue;
        if (ntypes == MAX_TYPES) fail(line, "%s declares more than %d types", include_name, MAX_TYPES);
        strcpy(types[ntypes].name, name);
        types[ntypes++].what = star ? H_POINTER : what;
    }
    free(text);
}

/* include "PATH": the header that declares the types the hooks' pointers name, which the generated header includes */
static void parse_include(const char* p, int line, int first_line) {
    size_t n = 0;
    if (include_name[0]) fail(line, "a spec has one include line, on line %d", first_line);
    p = expect(p, "\"", line);
    while (*p && *p != '"') {
        if (n + 1 >= MAX_NAME) fail(line, "the header's name is too long");
        include_name[n++] = *p++;
    }
    include_name[n] = 0;
    if (*p != '"' || !n) fail(line, "expected the header's name in quotes");
    if (*skip_space(p + 1)) fail(line, "unexpected text after the header's name");
    read_header(line);
}

/* whether p starts with the keyword const */
static int const_word(const char* p) { return !strncmp(p, "const", 5) && !isalnum((unsigned char)p[5]) && p[5] != '_'; }

/* a type: a kind, a type of the include line's header, or either followed by '*'s (each optionally followed by
   const) after an optional const, for an argument or a result (what). Its kind (T_ADDR for a pointer) and its C type
   for the function type into cname; the text after it */
static const char* parse_type(const char* p, Kind* kind, char* cname, int line, const char* what) {
    char base[MAX_NAME];
    int is_const = 0, stars = 0, k = -1, t = -1, i;
    p = skip_space(p);
    if (const_word(p)) is_const = 1, p += 5;
    p = word(p, base, line, what);
    for (i = 0; k < 0 && i < (int)(sizeof kinds / sizeof *kinds); i++)
        if (!strcmp(kinds[i].name, base)) k = i;
    for (i = 0; k < 0 && t < 0 && i < ntypes; i++)
        if (!strcmp(types[i].name, base)) t = i;
    if (k < 0 && t < 0 && strcmp(base, "char")) fail(line, "unknown type '%s'", base);
    snprintf(cname, MAX_NAME, "%s%s", is_const ? "const " : "", k >= 0 ? kinds[k].cname : base);
    for (;;) {
        const char* q = skip_space(p);
        size_t n = strlen(cname);
        if (*q == '*') {
            if (n + 2 > MAX_NAME) fail(line, "the %s is too long", what);
            strcpy(cname + n, "*");
            stars++;
            p = q + 1;
        } else if (stars && const_word(q) && *skip_space(q + 5) == '*') {
            if (n + 7 > MAX_NAME) fail(line, "the %s is too long", what);
            strcpy(cname + n, " const");
            p = q + 5;
        } else {
            break;
        }
    }
    if (stars && const_word(skip_space(p)))
        fail(line, "a const after the last '*' of %s says nothing the guest's caller sees: drop it", cname);
    if (stars) {
        *kind = T_ADDR;
        return p;
    }
    if (is_const) fail(line, "const %s has no '*' after it: a value isn't const to its caller", base);
    if (k >= 0) {
        *kind = (Kind)k;
        return p;
    }
    if (t < 0 || types[t].what == H_OTHER)
        fail(line, "%s is passed by value, but it isn't %s's enum or pointer: write the type of its size (i32, u64...)",
             base, include_name[0] ? include_name : "the include line");
    *kind = types[t].what == H_ENUM ? T_I32 : T_ADDR; /* an enum is an int under AAPCS64 */
    return p;
}

/* the handler a symbol names: a '_' before its first capital, after a part in lower case (nvnDeviceFinalize:
   nvn_DeviceFinalize), into buf. 0 when the symbol has no such part */
static int derived_handler(const char* symbol, char* buf) {
    size_t i = 0;
    if (!islower((unsigned char)symbol[0])) return 0;
    while (islower((unsigned char)symbol[i]) || isdigit((unsigned char)symbol[i])) i++;
    if (!isupper((unsigned char)symbol[i]) || strlen(symbol) + 2 > MAX_NAME) return 0;
    memcpy(buf, symbol, i);
    buf[i] = '_';
    strcpy(buf + i + 1, symbol + i);
    return is_identifier(buf);
}

/* a version of key, its parts packed as hook.h's hook_version_text reads them */
static const char* version(const char* p, int key, unsigned long* v, int line) {
    char text[64];
    const char* s = text;
    unsigned long part[3] = {0, 0, 0};
    int nparts = 0, most = key == K_SDK ? 3 : 2;
    size_t n = 0;
    p = skip_space(p);
    while (isalnum((unsigned char)*p) || *p == '.' || *p == '_') {
        if (n + 1 >= sizeof text) fail(line, "malformed version: too long");
        text[n++] = *p++;
    }
    text[n] = 0;
    if (!n) fail(line, "expected a version");
    for (;;) {
        int digits = 0;
        if (nparts == most)
            fail(line, "malformed version '%s': an %s version has at most %d parts", text, key_name[key], most);
        while (isdigit((unsigned char)*s)) {
            part[nparts] = part[nparts] * 10 + (unsigned long)(*s++ - '0');
            if (part[nparts] > 65535) fail(line, "malformed version '%s': a part is too large", text);
            digits++;
        }
        if (!digits) fail(line, "malformed version '%s'", text);
        nparts++;
        if (!*s) break;
        if (*s++ != '.') fail(line, "malformed version '%s'", text);
    }
    if (key == K_SDK) {
        if (part[0] > 255 || part[1] > 255 || part[2] > 255)
            fail(line, "malformed version '%s': an sdk version's parts are at most 255", text);
        *v = part[0] << 16 | part[1] << 8 | part[2];
    } else {
        if (part[0] > 255) fail(line, "malformed version '%s': an nvn major version is at most 255", text);
        *v = part[0] << 16 | part[1];
    }
    return p;
}

/* @version([KEY] OP VERSION, ...), p after "@version": the text after its ')' */
static const char* parse_version(const char* p, Range* r, int line) {
    char key[MAX_NAME];
    p = expect(p, "(", line);
    p = skip_space(p);
    r->key = K_SDK;
    r->lo = 0;
    r->hi = UNBOUNDED;
    if (isalpha((unsigned char)*p)) {
        size_t n = 0;
        while (isalnum((unsigned char)*p) || *p == '_') {
            if (n + 1 >= sizeof key) fail(line, "the version key is too long");
            key[n++] = *p++;
        }
        key[n] = 0;
        if (!strcmp(key, "sdk"))
            r->key = K_SDK;
        else if (!strcmp(key, "nvn"))
            r->key = K_NVN;
        else
            fail(line, "unknown version key '%s' (sdk or nvn)", key);
    }
    for (;;) {
        unsigned long v;
        int op = 0;
        p = skip_space(p);
        if (!strncmp(p, ">=", 2))
            op = 0, p += 2;
        else if (!strncmp(p, "<=", 2))
            op = 1, p += 2;
        else if (!strncmp(p, "==", 2))
            op = 2, p += 2;
        else if (*p == '>')
            op = 3, p++;
        else if (*p == '<')
            op = 4, p++;
        else
            fail(line, "expected a comparison (>=, >, <=, <, ==)");
        p = version(p, r->key, &v, line);
        /* the bounds as [lo, hi): a packed version is below 2^24, so v + 1 can't reach UNBOUNDED */
        if ((op == 0 || op == 2) && v > r->lo) r->lo = v;
        if (op == 3 && v + 1 > r->lo) r->lo = v + 1;
        if ((op == 1 || op == 2) && v + 1 < r->hi) r->hi = v + 1;
        if (op == 4 && v < r->hi) r->hi = v;
        p = skip_space(p);
        if (*p == ')') break;
        if (*p != ',') fail(line, "expected ',' or ')'");
        p++;
    }
    if (r->lo >= r->hi) fail(line, "no version satisfies this @version");
    return p + 1;
}

/* an address argument's buffer, p at "in[", "out[" or "inout[": its length [NAME [* NUMBER]] [+ NUMBER] | NUMBER */
static const char* parse_buffer(const char* p, Arg* a, int line) {
    char mode[MAX_NAME];
    int i;
    p = word(p, mode, line, "buffer's use");
    for (i = B_IN; i <= B_INOUT && strcmp(mode, buf_mode[i]); i++) {
    }
    if (i > B_INOUT) fail(line, "expected in[...], out[...] or inout[...] after %s, not %s", a->name, mode);
    if (a->kind != T_ADDR) fail(line, "only an addr or a pointer argument is a buffer, not %s", a->name);
    a->mode = i;
    a->len_arg = -1;
    a->len_mul = 1;
    p = expect(p, "[", line);
    p = skip_space(p);
    if (isalpha((unsigned char)*p) || *p == '_') {
        p = word(p, a->len_name, line, "buffer's length");
        p = skip_space(p);
        if (*p == '*') p = number(p + 1, &a->len_mul, line, "length's factor");
        p = skip_space(p);
        if (*p == '+') p = number(p + 1, &a->len_add, line, "length's addend");
    } else {
        p = number(p, &a->len_add, line, "buffer's length");
    }
    return expect(p, "]", line);
}

/* the arguments the buffers' lengths name, which may come after them: integers in x registers */
static void resolve_buffers(Hook* h, int line) {
    int i, j;
    for (i = 0; i < h->nargs; i++) {
        Arg* a = &h->args[i];
        if (!a->mode) continue;
        if (++h->nbufs > 4) fail(line, "a hook has at most 4 buffers");
        if (!a->len_name[0]) continue;
        for (j = 0; j < h->nargs && strcmp(h->args[j].name, a->len_name); j++) {
        }
        if (j == h->nargs) fail(line, "%s's length names %s, which isn't an argument", a->name, a->len_name);
        if (h->args[j].kind < T_U8 || h->args[j].kind > T_I64)
            fail(line, "%s's length %s is not an integer", a->name, a->len_name);
        a->len_arg = j;
    }
    if (h->nbufs && !h->pure) fail(line, "only a pure hook's buffers are checked: say pure, or drop the buffers");
}

/* hook SYMBOL(TYPE NAME, ...) [-> TYPE] [= HANDLER] [uses STRUCT, ...] [pure] */
static void parse_hook(const char* p, int line, const Range* range) {
    Hook* h;
    char derived[MAX_NAME];
    int x = 0, v = 0;
    if (nhooks == MAX_HOOKS) fail(line, "more than %d hooks", MAX_HOOKS);
    if (!nfamilies) fail(line, "a hook comes after a family line, which names its family");
    h = &hooks[nhooks++];
    memset(h, 0, sizeof *h);
    h->line = line;
    h->family = nfamilies - 1;
    h->range = *range;
    p = word(p, h->symbol, line, "symbol");
    p = expect(p, "(", line);
    p = skip_space(p);
    while (*p != ')') {
        Arg* a;
        if (h->nargs == MAX_ARGS) fail(line, "more than %d arguments", MAX_ARGS);
        a = &h->args[h->nargs++];
        p = parse_type(p, &a->kind, a->cname, line, "argument's type");
        if (a->kind == T_VOID) fail(line, "an argument can't be void");
        p = word(p, a->name, line, "argument's name");
        if (!is_identifier(a->name) || !strcmp(a->name, "c")) fail(line, "'%s' can't name an argument", a->name);
        a->reg = is_float(a->kind) ? v++ : x++;
        if (x > NREGS || v > NREGS) fail(line, "an argument past x7 or v7 would be on the stack, which no hook reads");
        p = skip_space(p);
        if (*p == '[')
            fail(line, "%s is an array, which the language doesn't take: write a pointer to its first element",
                 a->name);
        if (isalpha((unsigned char)*p)) p = parse_buffer(p, a, line);
        if (*p == ',')
            p = skip_space(p + 1);
        else if (*p != ')')
            fail(line, "expected ',' or ')'");
    }
    p = skip_space(p + 1);
    h->ret = T_VOID;
    strcpy(h->ret_cname, "void");
    if (!strncmp(p, "->", 2)) p = parse_type(p + 2, &h->ret, h->ret_cname, line, "result's type");
    p = skip_space(p);
    if (*p == '=') {
        p = word(p + 1, h->handler, line, "handler");
        if (!is_identifier(h->handler)) fail(line, "'%s' can't name a handler", h->handler);
        if (derived_handler(h->symbol, derived) && !strcmp(derived, h->handler))
            fail(line, "%s is the handler that %s names already: drop '= %s'", h->handler, h->symbol, h->handler);
    } else if (!derived_handler(h->symbol, h->handler)) {
        fail(line, "%s names no handler (no capital follows a part in lower case): give one with '= HANDLER'",
             h->symbol);
    }
    p = skip_space(p);
    if (keyword(p, "uses")) {
        p += 4;
        for (;;) {
            const char* s = skip_space(p);
            size_t n = 0;
            int i;
            if (h->nuses == MAX_USES) fail(line, "a handler uses at most %d structs", MAX_USES);
            while (isalnum((unsigned char)*s) || *s == '_' || *s == ':') {
                if (n + 1 >= MAX_NAME) fail(line, "the struct's name is too long");
                h->use_name[h->nuses][n++] = *s++;
            }
            h->use_name[h->nuses][n] = 0;
            if (!n) fail(line, "expected the struct a handler uses");
            for (i = 0; i < h->nuses; i++)
                if (!strcmp(h->use_name[i], h->use_name[h->nuses])) fail(line, "%s is used twice", h->use_name[i]);
            h->nuses++;
            p = skip_space(s);
            if (*p != ',') break;
            p++;
        }
    }
    if (keyword(p, "pure")) {
        h->pure = 1;
        p = skip_space(p + 4);
    }
    if (*p) fail(line, "unexpected text after the hook's handler");
    resolve_buffers(h, line);
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

/* route SYMBOL: a function the runtime wraps itself, routed through its slot as a hook's is */
static void parse_route(const char* p, int line) {
    if (nroutes == MAX_ROUTES) fail(line, "more than %d route lines", MAX_ROUTES);
    p = word(p, routes[nroutes], line, "symbol");
    if (*skip_space(p)) fail(line, "unexpected text after the symbol");
    route_lines[nroutes++] = line;
}

/* struct NAME [@version(...)] {: the variant whose fields follow */
static const char* parse_struct(const char* p, int line, const Range* range, int versioned) {
    Variant* s;
    size_t n = 0, c = 0;
    if (nvariants == MAX_VARIANTS) fail(line, "more than %d struct declarations", MAX_VARIANTS);
    s = &variants[nvariants++];
    memset(s, 0, sizeof *s);
    s->line = line;
    s->range = *range;
    p = skip_space(p);
    for (;;) { /* identifiers joined by :: */
        size_t start = n;
        while (isalnum((unsigned char)*p) || *p == '_') {
            if (n + 1 >= MAX_NAME) fail(line, "the struct's name is too long");
            s->cname[c++] = *p;
            s->name[n++] = *p++;
        }
        if (n == start || isdigit((unsigned char)s->name[start])) fail(line, "expected the struct's name");
        if (strncmp(p, "::", 2)) break;
        if (n + 3 >= MAX_NAME) fail(line, "the struct's name is too long");
        s->name[n++] = ':', s->name[n++] = ':', s->cname[c++] = '_';
        p += 2;
    }
    s->name[n] = 0, s->cname[c] = 0;
    p = skip_space(p);
    if (!strncmp(p, "@version", 8)) {
        if (versioned) fail(line, "a struct has one @version");
        p = parse_version(p + 8, &s->range, line);
    }
    return expect(p, "{", line);
}

/* fields of the open struct: TYPE NAME @OFFSET, separated by ; or line ends, until its "} size N". 1 once closed */
static int parse_fields(const char* p, int line) {
    Variant* s = &variants[nvariants - 1];
    int i;
    for (;;) {
        Field* f;
        char type[MAX_NAME];
        p = skip_space(p);
        if (*p == ';') {
            p++;
            continue;
        }
        if (!*p) return 0;
        if (*p == '}') break;
        if (s->nfields == MAX_FIELDS) fail(line, "a struct has at most %d fields", MAX_FIELDS);
        f = &s->fields[s->nfields++];
        p = word(p, type, line, "field's type");
        f->kind = find_kind(type, line);
        if (f->kind == T_VOID) fail(line, "a field can't be void");
        p = word(p, f->name, line, "field's name");
        if (!is_identifier(f->name) || !strcmp(f->name, "size")) fail(line, "'%s' can't name a field", f->name);
        for (i = 0; i < s->nfields - 1; i++)
            if (!strcmp(s->fields[i].name, f->name)) fail(line, "struct %s has two fields %s", s->name, f->name);
        p = expect(p, "@", line);
        p = number(p, &f->offset, line, "field's offset");
        p = skip_space(p);
        if (*p && *p != ';' && *p != '}') fail(line, "expected ';', '}' or the end of the line after a field");
    }
    p = skip_space(p + 1);
    if (!keyword(p, "size")) fail(line, "expected 'size' after the struct's '}'");
    p = number(p + 4, &s->size, line, "struct's size");
    if (*skip_space(p)) fail(line, "unexpected text after the struct's size");
    if (!s->nfields) fail(line, "struct %s has no fields", s->name);
    for (i = 0; i < s->nfields; i++)
        if (s->fields[i].offset + kinds[s->fields[i].kind].bytes > s->size)
            fail(line, "field %s runs past the end of struct %s (size 0x%lx)", s->fields[i].name, s->name, s->size);
    return 1;
}

static void read_spec(const char* path) {
    char buf[2048];
    int line = 0, versioned = 0, version_line = 0, open = 0, include_line = 0;
    Range range = {K_ANY, 0, UNBOUNDED}, any = {K_ANY, 0, UNBOUNDED};
    FILE* f = fopen(path, "r");
    if (!f) fail(0, "cannot open %s", path);
    while (fgets(buf, sizeof buf, f)) {
        const char* p = skip_space(buf);
        size_t n = strlen(buf);
        line++;
        if (n && buf[n - 1] != '\n' && !feof(f)) fail(line, "line too long");
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (!*p || *p == '#') continue;
        if (open) {
            open = !parse_fields(p, line);
            continue;
        }
        if (!strncmp(p, "@version", 8)) {
            if (versioned) fail(line, "two @version for one declaration (line %d)", version_line);
            p = skip_space(parse_version(p + 8, &range, line));
            versioned = 1, version_line = line;
            if (!*p) continue;
        }
        if (keyword(p, "hook")) {
            parse_hook(p + 4, line, versioned ? &range : &any);
        } else if (keyword(p, "struct")) {
            open = !parse_fields(parse_struct(p + 6, line, versioned ? &range : &any, versioned), line);
        } else if (keyword(p, "family")) {
            if (versioned) fail(version_line, "@version goes before a hook or a struct");
            parse_family(p + 6, line);
        } else if (keyword(p, "route")) {
            if (versioned) fail(version_line, "@version goes before a hook or a struct");
            parse_route(p + 5, line);
        } else if (keyword(p, "include")) {
            if (versioned) fail(version_line, "@version goes before a hook or a struct");
            parse_include(p + 7, line, include_line);
            include_line = line;
        } else {
            fail(line, "expected a family, a hook, a struct, a route, an include or a comment");
        }
        versioned = 0;
    }
    fclose(f);
    if (open) fail(variants[nvariants - 1].line, "struct %s isn't closed", variants[nvariants - 1].name);
    if (versioned) fail(version_line, "@version goes before a hook or a struct");
}

/* ============================================================ checking and splitting */
static void range_text(const Range* r, char* buf, size_t n) {
    char lo[32], hi[32];
    unsigned long v[2];
    int i;
    if (r->key == K_ANY) {
        snprintf(buf, n, "any version");
        return;
    }
    v[0] = r->lo, v[1] = r->hi;
    for (i = 0; i < 2; i++) {
        char* t = i ? hi : lo;
        if (r->key == K_SDK)
            snprintf(t, sizeof lo, "%lu.%lu.%lu", v[i] >> 16, v[i] >> 8 & 255, v[i] & 255);
        else
            snprintf(t, sizeof lo, "%lu.%lu", v[i] >> 16, v[i] & 0xFFFF);
    }
    if (r->hi == r->lo + 1)
        snprintf(buf, n, "%s == %s", key_name[r->key], lo);
    else if (r->hi == UNBOUNDED)
        snprintf(buf, n, "%s >= %s", key_name[r->key], lo);
    else if (!r->lo)
        snprintf(buf, n, "%s < %s", key_name[r->key], hi);
    else
        snprintf(buf, n, "%s >= %s, < %s", key_name[r->key], lo, hi);
}

/* two declarations of one hook's symbol or one struct (what), b after a: refused unless both are versioned, by one
   key, for versions that don't overlap */
static void check_ranges(const char* what, const Range* a, int a_line, const Range* b, int b_line) {
    if (a->key == K_ANY && b->key == K_ANY) fail(b_line, "%s is declared on line %d too", what, a_line);
    if (a->key == K_ANY)
        fail(b_line, "%s is declared without @version on line %d, so it has no other declaration", what, a_line);
    if (b->key == K_ANY) fail(b_line, "%s is versioned on line %d, so every declaration of it is", what, a_line);
    if (a->key != b->key) fail(b_line, "%s is versioned by %s on line %d", what, key_name[a->key], a_line);
    if (a->lo < b->hi && b->lo < a->hi) fail(b_line, "%s's versions overlap line %d's", what, a_line);
}

static int by_range(const Range* a, const Range* b) { return a->lo < b->lo ? -1 : a->lo > b->lo; }
static int by_variant(const void* a, const void* b) {
    const Variant *x = (const Variant*)a, *y = (const Variant*)b;
    int order = x->first - y->first;
    return order ? order : by_range(&x->range, &y->range);
}

/* each struct's declarations together (in the order the structs are first declared) and in version order, checked
   against each other */
static void check_structs(void) {
    int i, j, k;
    for (i = 0; i < nvariants; i++) {
        Variant* b = &variants[i];
        char what[MAX_NAME + 8];
        b->first = i;
        snprintf(what, sizeof what, "struct %s", b->name);
        for (j = 0; j < i; j++) {
            const Variant* a = &variants[j];
            if (strcmp(a->name, b->name)) {
                if (!strcmp(a->cname, b->cname))
                    fail(b->line, "struct %s's C name is struct %s's (line %d)", b->name, a->name, a->line);
                continue;
            }
            if (b->first == i) b->first = j;
            check_ranges(what, &a->range, a->line, &b->range, b->line);
            if (j != b->first) continue;
            for (k = 0; k < b->nfields; k++) { /* the same fields as the first declaration's */
                int m;
                for (m = 0; m < a->nfields && strcmp(a->fields[m].name, b->fields[k].name); m++) {
                }
                if (m == a->nfields)
                    fail(b->line, "%s has a field %s, which line %d's hasn't", what, b->fields[k].name, a->line);
                if (a->fields[m].kind != b->fields[k].kind)
                    fail(b->line, "%s's field %s is a %s, and a %s on line %d", what, b->fields[k].name,
                         kinds[b->fields[k].kind].name, kinds[a->fields[m].kind].name, a->line);
            }
            if (a->nfields != b->nfields) {
                for (k = 0; k < a->nfields; k++) {
                    int m;
                    for (m = 0; m < b->nfields && strcmp(a->fields[k].name, b->fields[m].name); m++) {
                    }
                    if (m == b->nfields)
                        fail(b->line, "%s has no field %s, which line %d's has", what, a->fields[k].name, a->line);
                }
            }
        }
    }
    qsort(variants, (size_t)nvariants, sizeof *variants, by_variant);
    for (i = 0; i < nvariants; i++) {
        int same = i && !strcmp(variants[i - 1].name, variants[i].name);
        variants[i].index = same ? variants[i - 1].index + 1 : 0;
        variants[i].first = same ? variants[i - 1].first : i;
    }
}

static int by_symbol(const void* a, const void* b) {
    const Hook *x = (const Hook*)a, *y = (const Hook*)b;
    int order = strcmp(x->symbol, y->symbol);
    return order ? order : by_range(&x->range, &y->range);
}

/* each symbol's declarations against each other; then each hook's uses resolved, and its versions split at the bounds
   of the variants it uses into the entries */
static void check_hooks(void) {
    int i, j, u;
    for (i = 0; i < nhooks; i++)
        for (j = 0; j < i; j++)
            if (!strcmp(hooks[i].symbol, hooks[j].symbol)) {
                const Hook *a = hooks[i].line < hooks[j].line ? &hooks[i] : &hooks[j],
                           *b = a == &hooks[i] ? &hooks[j] : &hooks[i];
                if (a->range.key == K_ANY && b->range.key == K_ANY)
                    fail(b->line, "%s is hooked on line %d too", b->symbol, a->line);
                check_ranges(b->symbol, &a->range, a->line, &b->range, b->line);
            }
    for (i = 0; i < nroutes; i++) {
        for (j = 0; j < i; j++)
            if (!strcmp(routes[i], routes[j]))
                fail(route_lines[i], "%s is routed on line %d too", routes[i], route_lines[j]);
        for (j = 0; j < nhooks; j++)
            if (!strcmp(routes[i], hooks[j].symbol))
                fail(route_lines[i], "%s is hooked on line %d, which routes it already", routes[i], hooks[j].line);
    }
    qsort(hooks, (size_t)nhooks, sizeof *hooks, by_symbol);
    for (i = 0; i < nhooks; i++) {
        Hook* h = &hooks[i];
        int first = nentries, end;
        for (u = 0; u < h->nuses; u++) {
            for (j = 0; j < nvariants && strcmp(variants[j].name, h->use_name[u]); j++) {
            }
            if (j == nvariants) fail(h->line, "unknown struct '%s'", h->use_name[u]);
            h->use[u] = j;
        }
        if (nentries == MAX_ENTRIES) fail(h->line, "more than %d hooks once split by version", MAX_ENTRIES);
        entries[nentries].hook = i;
        entries[nentries].range = h->range;
        nentries++;
        /* each use splits the entries so far at its variants' bounds */
        for (u = 0; u < h->nuses; u++) {
            int start = nentries;
            end = nentries;
            for (j = first; j < end; j++) {
                int v;
                for (v = h->use[u]; v < nvariants && !strcmp(variants[v].name, h->use_name[u]); v++) {
                    const Range *a = &entries[j].range, *b = &variants[v].range;
                    Range r;
                    if (a->key != K_ANY && b->key != K_ANY && a->key != b->key)
                        fail(h->line, "%s's versions are %s's and struct %s's (line %d) %s's", h->symbol,
                             key_name[a->key], h->use_name[u], variants[v].line, key_name[b->key]);
                    r.key = a->key != K_ANY ? a->key : b->key;
                    r.lo = a->lo > b->lo ? a->lo : b->lo;
                    r.hi = a->hi < b->hi ? a->hi : b->hi;
                    if (r.lo >= r.hi) continue;
                    if (nentries == MAX_ENTRIES) fail(h->line, "more than %d hooks once split by version", MAX_ENTRIES);
                    entries[nentries] = entries[j];
                    entries[nentries].range = r;
                    entries[nentries].variant[u] = v;
                    nentries++;
                }
            }
            /* the split entries replace those they came from */
            memmove(&entries[first], &entries[start], sizeof *entries * (size_t)(nentries - start));
            nentries = first + (nentries - start);
            if (nentries == first)
                fail(h->line, "no declaration of struct %s holds for %s's versions", h->use_name[u], h->symbol);
        }
    }
}

/* two handlers of one name must take the same arguments and layouts: the header declares it once */
static void check_handlers(void) {
    int i, j, k;
    for (i = 0; i < nhooks; i++)
        for (j = 0; j < i; j++) {
            const Hook *a = &hooks[i], *b = &hooks[j];
            int same = a->ret == b->ret && a->nargs == b->nargs && a->nuses == b->nuses;
            if (strcmp(a->handler, b->handler)) continue;
            for (k = 0; same && k < a->nargs; k++)
                same = !strcmp(kinds[a->args[k].kind].ctype, kinds[b->args[k].kind].ctype);
            for (k = 0; same && k < a->nuses; k++) same = !strcmp(a->use_name[k], b->use_name[k]);
            if (!same)
                fail(a->line > b->line ? a->line : b->line, "%s takes other arguments on line %d", a->handler,
                     a->line > b->line ? b->line : a->line);
        }
}

/* with an include line, each symbol's function type: an identifier's, and one for all its declarations */
static void check_function_types(void) {
    int i, k;
    if (!include_name[0]) return;
    for (i = 0; i < nhooks; i++) {
        const Hook *a = &hooks[i], *b = i ? &hooks[i - 1] : NULL; /* sorted by symbol */
        int same;
        if (!is_identifier(a->symbol)) fail(a->line, "%s can't name a function type", a->symbol);
        if (!b || strcmp(a->symbol, b->symbol)) continue;
        same = !strcmp(a->ret_cname, b->ret_cname) && a->nargs == b->nargs;
        for (k = 0; same && k < a->nargs; k++) same = !strcmp(a->args[k].cname, b->args[k].cname);
        if (!same)
            fail(a->line > b->line ? a->line : b->line,
                 "%s takes other types on line %d: its function type is one for every version", a->symbol,
                 a->line > b->line ? b->line : a->line);
    }
}

/* ============================================================ the output */
/* the generated tables' names: hook_symbol, HOOK_COUNT, HookLayout_ and the rest, or another prefix's (--prefix nvn:
   nvn_symbol, NVN_COUNT, NvnLayout_) for a second spec beside the hooks', such as the NVN functions the runtime
   implements */
static const char* prefix = "hook";
static char upper[MAX_NAME], capital[MAX_NAME];

static void prototype(const Hook* h) {
    int i;
    out("%s %s(CPU* c", kinds[h->ret].ctype, h->handler);
    for (i = 0; i < h->nuses; i++) {
        const Variant* s = &variants[h->use[i]];
        out(", const %sLayout_%s* %s", capital, s->cname, s->cname);
    }
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
static void adapter(const Entry* e, int index) {
    const Hook* h = &hooks[e->hook];
    char range[96];
    int i;
    range_text(&e->range, range, sizeof range);
    out("/* %s, %s */\n", h->symbol, range);
    out("static void %s_fn_%d(CPU* RESTRICT c, u64 entry) {\n", prefix, index);
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
    for (i = 0; i < h->nuses; i++) {
        const Variant* s = &variants[e->variant[i]];
        out(", &%s_layout_%s[%d]", prefix, s->cname, s->index);
    }
    for (i = 0; i < h->nargs; i++) {
        out(", ");
        argument(&h->args[i]);
    }
    out(h->ret == T_VOID ? ");\n" : "));\n");
    out("    c->pc = c->x[30];\n");
    out("}\n");
}

/* the hooked functions' types, PFN<SYMBOL>PROC, as the guest calls them */
static void function_types(void) {
    int i, k;
    out("\n/* each hooked function's type as the guest calls it, for the reader: nothing calls through it, as a guest "
        "function\n   runs at a guest address. An integer has its guest size, and a pointer, a host pointer here, the "
        "guest's 8 bytes */\n");
    out("typedef char %s_pointer_size[sizeof(void*) == 8 ? 1 : -1];\n", prefix);
    for (i = 0; i < nhooks; i++) {
        const Hook* h = &hooks[i];
        char name[MAX_NAME];
        if (i && !strcmp(hooks[i - 1].symbol, h->symbol)) continue; /* its other versions take the same types */
        for (k = 0; h->symbol[k]; k++) name[k] = (char)toupper((unsigned char)h->symbol[k]);
        name[k] = 0;
        out("typedef %s (*PFN%sPROC)(", h->ret_cname, name);
        for (k = 0; k < h->nargs; k++) out("%s%s %s", k ? ", " : "", h->args[k].cname, h->args[k].name);
        out("%s);\n", h->nargs ? "" : "void");
    }
}

static void write_header(const char* path) {
    int i, j;
    out("/* Generated by hookgen from %s: the structs' layouts and the hooks' handlers. Don't edit */\n", spec_path);
    out("#ifndef SNX_%s_HOOK_H\n#define SNX_%s_HOOK_H\n", upper, upper);
    out("#include \"nx/hle/hook/hook.h\"\n");
    if (include_name[0]) out("#include \"%s\"\n", include_name);
    out("\n");
    for (i = 0; i < nvariants; i++) {
        const Variant* s = &variants[i];
        if (s->index) continue;
        out("/* %s: its size and its fields' offsets, in the variant for a module's version */\n", s->name);
        out("typedef struct {\n    u32 size;\n");
        for (j = 0; j < variants[s->first].nfields; j++) { /* in the order of the struct's first declaration */
            const Field* f = &variants[s->first].fields[j];
            out("    HookField_%s %s;\n", kinds[f->kind].name, f->name);
        }
        out("} %sLayout_%s;\n\n", capital, s->cname);
    }
    for (i = 0; i < nhooks; i++) {
        for (j = 0; j < i && strcmp(hooks[j].handler, hooks[i].handler); j++) {
        }
        if (j == i) prototype(&hooks[i]);
    }
    if (include_name[0]) function_types();
    out("#endif\n");
    flush(path);
}

static void write_table(const char* path) {
    int i, j, k;
    out("/* Generated by hookgen from %s: the structs' variants, the hooks' adapters and their table, sorted by symbol "
        "and version. Don't edit */\n",
        spec_path);
    out("#define %s_COUNT %d\n#define %s_FAMILIES %d\n\n", upper, nentries, upper, nfamilies);
    for (i = 0; i < nvariants; i = j) {
        const Variant* s = &variants[i];
        for (j = i; j < nvariants && !strcmp(variants[j].name, s->name); j++) {
        }
        out("/* %s, by version */\n", s->name);
        out("static const %sLayout_%s %s_layout_%s[%d] = {\n", capital, s->cname, prefix, s->cname, j - i);
        for (k = i; k < j; k++) {
            const Variant* v = &variants[k];
            char range[96];
            range_text(&v->range, range, sizeof range);
            out("    {0x%lx", v->size);
            for (int f = 0; f < variants[s->first].nfields; f++) {
                const Field* field = &variants[s->first].fields[f];
                int m;
                for (m = 0; strcmp(v->fields[m].name, field->name); m++) {
                }
                out(", {0x%lx}", v->fields[m].offset);
            }
            out("}, /* line %d: %s */\n", v->line, range);
        }
        out("};\n");
    }
    if (nvariants) out("\n");
    for (i = 0; i < nentries; i++) adapter(&entries[i], i);
    out("\nstatic const char* const %s_symbol[%s_COUNT + 1] = {\n", prefix, upper);
    for (i = 0; i < nentries; i++) out("    \"%s\",\n", hooks[entries[i].hook].symbol);
    out("    0};\nstatic const char* const %s_handler[%s_COUNT + 1] = {\n", prefix, upper);
    for (i = 0; i < nentries; i++) out("    \"%s\",\n", hooks[entries[i].hook].handler);
    out("    0};\nstatic const GuestFn %s_fn[%s_COUNT + 1] = {\n", prefix, upper);
    for (i = 0; i < nentries; i++) out("    %s_fn_%d,\n", prefix, i);
    out("    0};\nstatic const unsigned char %s_family[%s_COUNT + 1] = {\n", prefix, upper);
    for (i = 0; i < nentries; i++) out("    %d,\n", hooks[entries[i].hook].family);
    out("    0};\nstatic const unsigned char %s_key[%s_COUNT + 1] = {\n", prefix, upper);
    for (i = 0; i < nentries; i++) out("    %s,\n", key_macro[entries[i].range.key]);
    out("    0};\nstatic const u32 %s_lo[%s_COUNT + 1] = {\n", prefix, upper);
    for (i = 0; i < nentries; i++) out("    0x%lx,\n", entries[i].range.lo);
    out("    0};\nstatic const u32 %s_hi[%s_COUNT + 1] = {\n", prefix, upper);
    for (i = 0; i < nentries; i++) out("    0x%lx,\n", entries[i].range.hi);
    out("    0};\n/* the struct variants each passes, for the log */\nstatic const char* const %s_layouts[%s_COUNT + 1] = {\n",
        prefix, upper);
    for (i = 0; i < nentries; i++) {
        const Hook* h = &hooks[entries[i].hook];
        out("    \"");
        for (k = 0; k < h->nuses; k++) {
            const Variant* v = &variants[entries[i].variant[k]];
            out("%s%s of line %d", k ? ", " : "", v->name, v->line);
        }
        out("\",\n");
    }
    out("    0};\n/* verify mode (verify.h): the pure hooks' buffers and results */\n");
    out("static const HookVerify %s_verify[%s_COUNT + 1] = {\n", prefix, upper);
    for (i = 0; i < nentries; i++) {
        const Hook* h = &hooks[entries[i].hook];
        int n = 0;
        out("    {%d, %s, %d, {", h->pure, ret_macro[h->ret], h->nbufs);
        for (k = 0; k < h->nargs; k++) {
            const Arg* a = &h->args[k];
            if (!a->mode) continue;
            out("%s{%d, %d, %d, %lu, %lu}", n++ ? ", " : "", a->reg, a->mode,
                a->len_arg < 0 ? 0xFF : h->args[a->len_arg].reg, a->len_arg < 0 ? 0ul : a->len_mul, a->len_add);
        }
        out("%s}},\n", n ? "" : "{0, 0, 0, 0, 0}"); /* C99 has no empty initializer */
    }
    out("    {0, 0, 0, {{0, 0, 0, 0, 0}}}};\n");
    out("static const char* const %s_family_name[%s_FAMILIES + 1] = {\n", prefix, upper);
    for (i = 0; i < nfamilies; i++) out("    \"%s\",\n", families[i]);
    out("    0};\n");
    out("static const HookTable %s_table = {%s_COUNT, %s_symbol, %s_key, %s_lo, %s_hi};\n", prefix, upper, prefix,
        prefix, prefix, prefix);
    flush(path);
}

static int by_name(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* --routes: the symbols each spec's hooks and route lines name, each spec read and checked alone */
static void write_routes(const char* path, char** specs, int nspecs) {
    char** names = NULL;
    size_t count = 0, kept = 0, cap = 0, i;
    int s, k;
    for (s = 0; s < nspecs; s++) {
        spec_path = specs[s];
        nhooks = nvariants = nentries = nfamilies = nroutes = ntypes = 0;
        include_name[0] = 0;
        read_spec(spec_path);
        check_structs();
        check_hooks();
        for (k = 0; k < nhooks + nroutes; k++) {
            const char* name = k < nhooks ? hooks[k].symbol : routes[k - nhooks];
            if (count == cap) {
                cap = cap ? cap * 2 : 256;
                names = (char**)realloc(names, cap * sizeof *names);
                if (!names) fail(0, "out of memory");
            }
            names[count] = (char*)malloc(strlen(name) + 1);
            if (!names[count]) fail(0, "out of memory");
            strcpy(names[count++], name);
        }
    }
    qsort(names, count, sizeof *names, by_name);
    for (i = 0; i < count; i++) /* a symbol of several versions, or one that two specs name */
        if (!kept || strcmp(names[kept - 1], names[i]))
            names[kept++] = names[i];
        else
            free(names[i]);
    out("/* Generated by hookgen --routes: the symbols that a hook or a route line names, sorted by strcmp, which the "
        "recompiler calls through their dispatch slots. Don't edit */\n");
    out("#define HOOK_ROUTE_COUNT %lu\n", (unsigned long)kept);
    out("static const char* const hook_route_names[HOOK_ROUTE_COUNT + 1] = {\n");
    for (i = 0; i < kept; i++) {
        out("    \"%s\",\n", names[i]);
        free(names[i]);
    }
    out("    0};\n");
    free(names);
    flush(path);
}

int main(int argc, char** argv) {
    const char *header = NULL, *table = NULL, *routes_path = NULL;
    char* specs[MAX_SPECS];
    int i, nspecs = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--header") && i + 1 < argc)
            header = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            table = argv[++i];
        else if (!strcmp(argv[i], "--routes") && i + 1 < argc)
            routes_path = argv[++i];
        else if (!strcmp(argv[i], "--prefix") && i + 1 < argc)
            prefix = argv[++i];
        else if (!strcmp(argv[i], "-I") && i + 1 < argc && ninclude_dirs < MAX_DIRS)
            include_dirs[ninclude_dirs++] = argv[++i];
        else if (argv[i][0] != '-' && nspecs < MAX_SPECS)
            specs[nspecs++] = argv[i];
        else
            nspecs = 0, i = argc;
    }
    if (routes_path && nspecs && !header && !table) {
        write_routes(routes_path, specs, nspecs);
        free(obuf);
        return 0;
    }
    if (nspecs != 1 || (!header && !table)) {
        fprintf(stderr, "usage: hookgen [--prefix NAME] [-I DIR]... --header OUT.h --out OUT.inc SPEC\n"
                        "       hookgen [-I DIR]... --routes OUT.inc SPEC...\n");
        return 2;
    }
    spec_path = specs[0];
    for (i = 0; prefix[i]; i++) { /* an identifier, its macros' prefix the same in capitals */
        char ch = prefix[i];
        if (i == MAX_NAME - 1 || !(ch == '_' || (ch >= 'a' && ch <= 'z') || (i && ch >= '0' && ch <= '9'))) {
            fprintf(stderr, "hookgen: --prefix takes a lower-case identifier, not %s\n", prefix);
            return 2;
        }
        upper[i] = ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : ch;
        capital[i] = i ? ch : upper[i];
    }
    read_spec(spec_path);
    check_structs();
    check_hooks();
    check_handlers();
    check_function_types();
    if (header) write_header(header);
    if (table) write_table(table);
    free(obuf);
    return 0;
}
