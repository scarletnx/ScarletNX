/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx --mcp [--repo DIR]: an MCP server for an agent looking into a game's failure on this machine, as RFC #347
   has a player's own agent do. JSON-RPC 2.0 over stdin and stdout, a message a line (MCP's stdio transport), nothing
   else on stdout. Its tools read only this machine:

   - scan_log: the log summary (common/utilities/logscan.h) of a game's log or the launcher's Share Log file.
   - read_log: parts of a log by text (with lines of context) or by line range, or its last lines, never the whole of a
     large one.
   - find_game: the extracted games in the data folder's work/, with their title IDs and versions.
   - state_info: a recompiled game's save states (common/utilities/state_header.h): each one's slot, program, when it
     was saved, the play time then, the runtime and stamp that saved it, and whether this build can load it.
   - symbolize: game addresses (a backtrace's, a crash report's) named by the game's symbols, as tools/symbols.py does.
   - disassemble: a game function, by name or address range, with its branches named (disasm.h), as tools/disasm.py
     does, where this build has an A64 disassembler.
   - edenprobe_scaffold, edenprobe_check, edenprobe_run, edenprobe_finding (mcp_edenprobe.c): a confirmed failure as a
     new edenprobe case, written into a local checkout, checked on the host, run on this build, and written up.
   - gdb_attach, gdb_command, gdb_threads, gdb_guest, gdb_memory, gdb_detach (mcp_gdb.c, UNIX only): GDB on a game
     started with the GDB stub, the runtime's process read and driven through it.

   scan_log, read_log, symbolize and disassemble read only under the data folder (DIR, else snx_data_dir's) and its
   state, cache and settings folders: what a tool answers with is a log's text or a game's code, and those are where
   ScarletNX keeps them. The edenprobe tools work in the clone and on the NRO the person names, and answer only their
   own results. The GDB tools reach the game's process only, on this machine's loopback: gdb_command runs a list of
   GDB's reading, breakpoint and execution commands, none that runs a shell or a script or writes a file, though
   print and call evaluate the game's own functions.

   A game's code goes to the agent's context the way a session's tools/disasm.py output does, and nowhere else. An
   answer is cut at MAX_LINES lines: the agent's context is the scarce part */
#include "nx/runtime/driver/mcp.h"
#include "nx/runtime/driver/mcp_int.h"
#include "common/utilities/common.h"
#include "common/utilities/state_header.h"
#include "common/utilities/demangle.h"
#include "common/utilities/logscan.h"
#include "nx/loader/loader.h"
#include "nx/runtime/driver/disasm.h"
#include "nx/runtime/driver/hactool/cJSON.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES    400
#define LINE_CUT     400       /* a line read_log shows is cut here */
#define LINE_READ    (1 << 16) /* what read_log reads of a line at a time: the rest of a longer one is skipped */
#define MAX_DISTANCE 0x4000    /* a symbol further back than this isn't taken as the enclosing function (symbols.py) */
#define MAX_FUNCTION 0x10000   /* the most of a function disassemble shows */
#define MAX_CONTEXT  20
#define MAX_ROOTS    8

/* A game's symbols at their run-time addresses, sorted, from the recompiler's loader */
typedef struct {
    SnxU64 address;
    size_t module, symbol;
} Sym;
typedef struct {
    SnxProgram program;
    Sym* syms;
    size_t count;
} Game;

typedef struct {
    char* data;             /* the data folder */
    char* roots[MAX_ROOTS]; /* the folders read_log reads under */
    char* works[2];         /* the extractions: the cache's work/, then an older version's in the data folder */
    char* self;             /* this program, which edenprobe_run recompiles and runs edenprobe with */
    char* cache;            /* the data folder's cache folder, where edenprobe_run keeps its recompile */
    int nroots;
    /* the last game symbolize or disassemble loaded, kept for the next call: an agent asks about one game many times,
       and loading one takes a second or more. game_folder NULL for none */
    char* game_folder;
    Game game;
    char* gdb_game; /* the game folder gdb_attach named guest addresses by, or NULL */
} Mcp;

/* ============================================================ text (mcp_int.h) */
void mcp_put_n(Text* t, const char* p, size_t n) {
    if (t->n + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 1024;
        while (cap < t->n + n + 1) cap *= 2;
        t->p = (char*)snx_realloc(t->p, cap);
        t->cap = cap;
    }
    memcpy(t->p + t->n, p, n);
    t->n += n;
    t->p[t->n] = 0;
}
void mcp_put(Text* t, const char* p) { mcp_put_n(t, p, strlen(p)); }
static void hex(char* out, SnxU64 v) {
    char digits[17];
    int n = 0;
    do {
        digits[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    while (n) *out++ = digits[--n];
    *out = 0;
}
void mcp_put_hex(Text* t, SnxU64 v) {
    char h[17];
    hex(h, v);
    mcp_put(t, h);
}
void mcp_put_num(Text* t, unsigned long v) {
    char num[24];
    sprintf(num, "%lu", v);
    mcp_put(t, num);
}
/* hex digits, an optional 0x before them; 0 for text that isn't one */
static int parse_hex(const char* s, SnxU64* out) {
    SnxU64 v = 0;
    int n = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s && n < 16; s++, n++) {
        int c = tolower((unsigned char)*s);
        if (c >= '0' && c <= '9')
            v = v << 4 | (SnxU64)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v = v << 4 | (SnxU64)(c - 'a' + 10);
        else
            return 0;
    }
    if (!n || *s) return 0;
    *out = v;
    return 1;
}

/* ============================================================ the folders */
static void add_root(Mcp* m, char* path) {
    if (path && m->nroots < MAX_ROOTS)
        m->roots[m->nroots++] = path;
    else
        free(path);
}
static int mcp_open(Mcp* m, int argc, char** argv) {
    char *self, *dir, *work;
    int i;
    memset(m, 0, sizeof *m);
    for (i = 2; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--repo")) {
            if (!strcmp(argv[i], "--repo")) snx_option_renamed("--repo", "--data");
            m->data = snx_absolute(argv[++i]);
        }
    self = snx_executable(argv[0]);
    if (!m->data) {
        dir = self ? snx_dirname(self) : NULL;
        m->data = dir ? snx_data_dir(dir) : NULL;
        free(dir);
    }
    m->self = self;
    if (!m->data) return snx_error("--mcp: no data folder (name one with --repo)");
    add_root(m, snx_strdup(m->data));
    add_root(m, snx_user_path(m->data, SNX_USER_STATE));
    m->cache = snx_user_path(m->data, SNX_USER_CACHE);
    add_root(m, snx_strdup(m->cache));
    add_root(m, snx_user_path(m->data, SNX_USER_CONFIG));
    /* snx_work_dir without its notice, which would go to stdout */
    work = snx_path_join(m->data, "work");
    m->works[0] = snx_user_path(work, SNX_USER_CACHE);
    m->works[1] = work;
    return 0;
}
static void game_free(Game* g);
static void mcp_close(Mcp* m) {
    int i;
    mcp_gdb_close(); /* the game runs on */
    free(m->gdb_game);
    if (m->game_folder) game_free(&m->game);
    free(m->game_folder);
    for (i = 0; i < m->nroots; i++) free(m->roots[i]);
    free(m->works[0]);
    free(m->works[1]);
    free(m->self);
    free(m->cache);
    free(m->data);
}
/* path is under one of the roots, both resolved. A root that doesn't resolve (a folder not made yet) holds nothing:
   snx_path_within takes a path it can't resolve as within */
static int readable(const Mcp* m, const char* path) {
    char *full = snx_absolute(path), *root;
    int i, ok = 0;
    for (i = 0; full && i < m->nroots && !ok; i++) {
        root = snx_absolute(m->roots[i]);
        ok = root && snx_path_within(full, root);
        free(root);
    }
    free(full);
    return ok;
}
static const char outside[] = "outside the folders the tools read (ScarletNX's data folder and its state, cache and "
                              "settings folders)";

/* ============================================================ scan_log, read_log */
static char* scan_log(const char* path, Text* err) {
    FILE* f = fopen(path, "rb");
    SnxLogScan s;
    char* out = NULL;
    if (!f) {
        mcp_put(err, "the file cannot be read");
        return NULL;
    }
    if (!snx_logscan_file(&s, f))
        out = snx_logscan_json(&s, snx_demangle);
    else
        mcp_put(err, "out of memory or a read error");
    snx_logscan_free(&s);
    fclose(f);
    return out;
}

/* a line of the log as read_log shows it, its number first */
typedef struct {
    char text[LINE_CUT + 4];
    unsigned long number;
} Shown;
static void keep(Shown* s, const char* line, size_t n, unsigned long number) {
    if (n > LINE_CUT) {
        memcpy(s->text, line, LINE_CUT);
        strcpy(s->text + LINE_CUT, "...");
    } else {
        memcpy(s->text, line, n);
        s->text[n] = 0;
    }
    s->number = number;
}
static void show(Text* t, const Shown* s, unsigned long* last) {
    if (*last && s->number != *last + 1) {
        mcp_put(t, "--\n");
        t->lines++;
    }
    mcp_put_num(t, s->number);
    mcp_put(t, ": ");
    mcp_put(t, s->text);
    mcp_put(t, "\n");
    t->lines++;
    *last = s->number;
}

/* any of the |-separated texts in line, ignoring case when nocase */
static int matches(const char* line, size_t n, const char* pattern, int nocase) {
    const char* alt = pattern;
    while (*alt) {
        size_t len = strcspn(alt, "|"), i, j;
        for (i = 0; len && i + len <= n; i++) {
            for (j = 0; j < len; j++) {
                int a = (unsigned char)line[i + j], b = (unsigned char)alt[j];
                if (nocase ? tolower(a) != tolower(b) : a != b) break;
            }
            if (j == len) return 1;
        }
        alt += len;
        if (*alt) alt++;
    }
    return 0;
}

/* The log's lines that hold pattern with context lines around each, else count lines from start (1-based), else its
   last count lines. A line past the reader's buffer is shown cut: read_log is for logs */
static char* read_log(const char* path, const char* pattern, int nocase, int context, long start, int count,
                      Text* err) {
    char* line;
    Shown* ring;
    Text t = {0};
    FILE* f;
    unsigned long number = 0, last = 0, after = 0, more = 0;
    int slots = pattern ? context + 1 : count, n = 0;
    if (context < 0 || context > MAX_CONTEXT || count < 1 || count > MAX_LINES) {
        mcp_put(err, "context is 0 to 20 lines, count 1 to 400");
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        mcp_put(err, "the file cannot be read");
        return NULL;
    }
    ring = (Shown*)snx_alloc((size_t)slots * sizeof *ring);
    line = (char*)snx_alloc(LINE_READ);
    while (fgets(line, LINE_READ, f)) {
        size_t len = strlen(line);
        int whole = len && line[len - 1] == '\n';
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        if (!whole && !feof(f)) { /* the rest of a long line goes */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
        }
        number++;
        if (pattern) {
            if (matches(line, len, pattern, nocase)) {
                int k;
                if (t.lines >= MAX_LINES) {
                    more++;
                    continue;
                }
                for (k = n > context ? n - context : 0; k < n; k++) { /* the context before, not shown yet */
                    const Shown* s = &ring[k % slots];
                    if (s->number > last) show(&t, s, &last);
                }
                keep(&ring[n++ % slots], line, len, number);
                show(&t, &ring[(n - 1) % slots], &last);
                after = (unsigned long)context;
            } else {
                keep(&ring[n++ % slots], line, len, number);
                if (after && t.lines < MAX_LINES) {
                    show(&t, &ring[(n - 1) % slots], &last);
                    after--;
                }
            }
        } else if (start > 0) {
            if ((long)number >= start && (long)number < start + count) {
                keep(&ring[0], line, len, number);
                show(&t, &ring[0], &last);
            }
        } else
            keep(&ring[n++ % slots], line, len, number);
    }
    fclose(f);
    free(line);
    if (!pattern && start <= 0) {
        int k;
        for (k = n > slots ? n - slots : 0; k < n; k++) show(&t, &ring[k % slots], &last);
    }
    free(ring);
    {
        Text out = {0};
        mcp_put(&out, path);
        mcp_put(&out, " has ");
        mcp_put_num(&out, number);
        mcp_put(&out, " lines\n");
        if (t.p) mcp_put(&out, t.p);
        if (more) {
            mcp_put(&out, "(");
            mcp_put_num(&out, more);
            mcp_put(&out, " more matching lines: narrow the pattern)\n");
        }
        if (pattern && !t.lines) mcp_put(&out, "(no line holds it)\n");
        free(t.p);
        return out.p;
    }
}

/* ============================================================ the games */
static char* nacp_version(const char* game) {
    char *path = snx_path_join(game, "control.nacp"), *out = NULL;
    size_t size = 0, n;
    unsigned char* nacp = snx_read_file(path, &size);
    free(path);
    if (nacp && size >= 0x3070) {
        for (n = 0; n < 16 && nacp[0x3060 + n]; n++) {}
        out = (char*)snx_alloc(n + 1);
        memcpy(out, nacp + 0x3060, n);
        out[n] = 0;
    }
    free(nacp);
    return out;
}

/* every extracted game, or only title_id's, as {path, title_id, version} into list */
/* the save states of the recompiled game title_id, in the data folder's games/ or recompiled/ (state_header.h) */
static void find_states(const Mcp* m, SnxU64 title_id, cJSON* list) {
    static const char* const libraries[] = {"games", "recompiled"};
    char id[17];
    int l, k;
    hex(id, title_id);
    for (k = (int)strlen(id); k < 16; k++) {
        memmove(id + 1, id, (size_t)k + 1);
        id[0] = '0';
    }
    for (k = 0; id[k]; k++) id[k] = (char)toupper((unsigned char)id[k]);
    for (l = 0; l < 2; l++) {
        char *folder = snx_format("%s/%s/%s", m->data, libraries[l], id), *dir = snx_path_join(folder, "states");
        char** names = NULL;
        size_t count = 0, i;
        if (!snx_list_dir(dir, &names, &count))
            for (i = 0; i < count; i++) {
                char program[256], slot[16], stamp[64], why[256], *path = snx_path_join(dir, names[i]), *prog;
                SnxStateHeader h;
                cJSON* s;
                if (!snx_state_name(names[i], program, sizeof program, slot, sizeof slot) ||
                    snx_state_peek(path, &h, NULL, why, sizeof why)) {
                    free(path);
                    continue;
                }
                prog = snx_path_join(folder, program);
                snx_state_stamp(prog, stamp, sizeof stamp);
                s = cJSON_CreateObject();
                cJSON_AddStringToObject(s, "path", path);
                cJSON_AddStringToObject(s, "program", program);
                cJSON_AddStringToObject(s, "slot", slot);
                cJSON_AddNumberToObject(s, "saved", (double)h.created);
                cJSON_AddNumberToObject(s, "play_time_s", (double)h.guest_ns / 1e9);
                cJSON_AddNumberToObject(s, "abi", h.abi);
                cJSON_AddStringToObject(s, "runtime", h.runtime);
                cJSON_AddStringToObject(s, "stamp", h.stamp);
                cJSON_AddStringToObject(s, "game", h.game);
                if (snx_state_fits(&h, title_id, stamp, why, sizeof why))
                    cJSON_AddStringToObject(s, "cannot_load", why);
                else
                    cJSON_AddNullToObject(s, "cannot_load");
                cJSON_AddItemToArray(list, s);
                free(prog);
                free(path);
            }
        snx_free_strings(names, count);
        free(dir);
        free(folder);
    }
}
static void find_games(const Mcp* m, SnxU64 title_id, cJSON* list) {
    int w;
    for (w = 0; w < 2; w++) {
        char** names = NULL;
        size_t count = 0, i;
        if (!m->works[w] || !snx_is_dir(m->works[w]) || (w && m->works[0] && !strcmp(m->works[0], m->works[1])))
            continue;
        if (snx_list_dir(m->works[w], &names, &count)) continue;
        for (i = 0; i < count; i++) {
            char *game = snx_path_join(m->works[w], names[i]), error[256], id[17], *version;
            SnxNpdm npdm;
            if (snx_is_dir(game) && !snx_load_npdm(game, &npdm, error, sizeof error) && npdm.title_id &&
                (!title_id || npdm.title_id == title_id)) {
                cJSON* g = cJSON_CreateObject();
                int k;
                hex(id, npdm.title_id);
                for (k = (int)strlen(id); k < 16; k++) { /* sixteen digits, upper case, as the logs' header */
                    memmove(id + 1, id, (size_t)k + 1);
                    id[0] = '0';
                }
                for (k = 0; id[k]; k++) id[k] = (char)toupper((unsigned char)id[k]);
                version = nacp_version(game);
                cJSON_AddStringToObject(g, "path", game);
                cJSON_AddStringToObject(g, "title_id", id);
                if (version)
                    cJSON_AddStringToObject(g, "version", version);
                else
                    cJSON_AddNullToObject(g, "version");
                cJSON_AddBoolToObject(g, "is64", npdm.is64);
                cJSON_AddItemToArray(list, g);
                free(version);
            }
            free(game);
        }
        snx_free_strings(names, count);
    }
}

/* a game named by its folder (or NRO), a folder of work/, or its title ID: the folder, malloc'd, or NULL with why in err.
   Only under ScarletNX's folders, as the logs: a game's code is read from where ScarletNX keeps it */
static char* game_found(const Mcp* m, const char* game, Text* err);
static char* game_folder(const Mcp* m, const char* game, Text* err) {
    char* found = game_found(m, game, err);
    if (found && !readable(m, found)) {
        mcp_put(err, outside);
        free(found);
        return NULL;
    }
    return found;
}
static char* game_found(const Mcp* m, const char* game, Text* err) {
    SnxU64 id;
    int w;
    if (strlen(game) == 16 && parse_hex(game, &id)) {
        cJSON* list = cJSON_CreateArray();
        char* out = NULL;
        find_games(m, id, list);
        if (cJSON_GetArraySize(list))
            out = snx_strdup(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list, 0), "path")->valuestring);
        else
            mcp_put(err, "no extracted game with that title ID in work/ (find_game lists them)");
        cJSON_Delete(list);
        return out;
    }
    if (snx_is_dir(game) || snx_is_file(game)) return snx_strdup(game); /* a folder, or an NRO */
    for (w = 0; w < 2; w++) {
        char* path = m->works[w] ? snx_path_join(m->works[w], game) : NULL;
        if (path && snx_is_dir(path)) return path;
        free(path);
    }
    mcp_put(err, "not a folder, a game of work/ or a title ID");
    return NULL;
}

static int by_address(const void* a, const void* b) {
    SnxU64 x = ((const Sym*)a)->address, y = ((const Sym*)b)->address;
    return x < y ? -1 : x > y;
}
static int game_load(Game* g, const char* folder, Text* err) {
    char error[1024];
    size_t i, j, n = 0;
    memset(g, 0, sizeof *g);
    if (snx_load_program(folder, &g->program, error, sizeof error)) {
        mcp_put(err, error);
        return -1;
    }
    for (i = 0; i < g->program.module_count; i++) n += g->program.modules[i].symbol_count;
    g->syms = (Sym*)snx_alloc((n ? n : 1) * sizeof *g->syms);
    for (i = 0; i < g->program.module_count; i++) {
        const SnxModuleImage* mi = &g->program.modules[i];
        for (j = 0; j < mi->symbol_count; j++) {
            SnxU64 v = mi->symbols[j].value;
            if (!mi->symbols[j].name || !mi->symbols[j].name[0] || !v) continue;
            /* a 32-bit program's Thumb functions have bit 0 set: the code starts at the even address */
            g->syms[g->count].address = mi->base + (g->program.npdm.is64 ? v : v & ~(SnxU64)1);
            g->syms[g->count].module = i;
            g->syms[g->count].symbol = j;
            g->count++;
        }
    }
    qsort(g->syms, g->count, sizeof *g->syms, by_address);
    return 0;
}
static void game_free(Game* g) {
    free(g->syms);
    snx_free_program(&g->program);
}
/* folder's game, loaded once for as long as calls ask about it. NULL with why in err */
static const Game* game_get(Mcp* m, const char* folder, Text* err) {
    char* full = snx_absolute(folder);
    if (!full) {
        mcp_put(err, "the game cannot be read");
        return NULL;
    }
    if (m->game_folder && !strcmp(m->game_folder, full)) {
        free(full);
        return &m->game;
    }
    if (m->game_folder) game_free(&m->game);
    free(m->game_folder);
    m->game_folder = NULL;
    if (game_load(&m->game, full, err)) {
        free(full);
        return NULL;
    }
    m->game_folder = full;
    return &m->game;
}
static const char* sym_name(const Game* g, const Sym* s) {
    return g->program.modules[s->module].symbols[s->symbol].name;
}
static const SnxModuleImage* module_at(const Game* g, SnxU64 a) {
    size_t i;
    for (i = 0; i < g->program.module_count; i++) {
        const SnxModuleImage* mi = &g->program.modules[i];
        if (a >= mi->base && a - mi->base < mi->size) return mi;
    }
    return NULL;
}
/* the last symbol at or before a, or NULL */
static const Sym* sym_before(const Game* g, SnxU64 a) {
    size_t lo = 0, hi = g->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g->syms[mid].address <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo ? &g->syms[lo - 1] : NULL;
}
static void put_name(Text* t, const char* name) {
    char* plain = !strncmp(name, "_Z", 2) ? snx_demangle(name) : NULL;
    mcp_put(t, plain ? plain : name);
    free(plain);
}
/* "<module> <name>+<off>", or "<module> ?" with the symbol it is past, as symbols.py's describe */
static void describe(Text* t, const Game* g, SnxU64 a) {
    const SnxModuleImage* mi = module_at(g, a);
    const Sym* s = sym_before(g, a);
    mcp_put(t, mi ? mi->name : "?");
    mcp_put(t, " ");
    if (s && a - s->address < MAX_DISTANCE) {
        put_name(t, sym_name(g, s));
        mcp_put(t, "+");
        mcp_put_hex(t, a - s->address);
    } else if (s) {
        mcp_put(t, "? (");
        put_name(t, sym_name(g, s));
        mcp_put(t, "+");
        mcp_put_hex(t, a - s->address);
        mcp_put(t, " is past MAX_DISTANCE)");
    } else
        mcp_put(t, "?");
}

static char* symbolize(Mcp* m, const char* game, const cJSON* addresses, Text* err) {
    char* folder = game_folder(m, game, err);
    const Game* g;
    Text t = {0};
    int i, n = cJSON_GetArraySize(addresses);
    if (!folder) return NULL;
    if (!n) {
        mcp_put(err, "addresses: hexadecimal game addresses, as the logs print them");
        free(folder);
        return NULL;
    }
    g = game_get(m, folder, err);
    free(folder);
    if (!g) return NULL;
    for (i = 0; i < n; i++) {
        const cJSON* a = cJSON_GetArrayItem(addresses, i);
        SnxU64 v;
        if (!cJSON_IsString(a) || !parse_hex(a->valuestring, &v)) {
            mcp_put(&t, cJSON_IsString(a) ? a->valuestring : "?");
            mcp_put(&t, " is not a hexadecimal address\n");
            continue;
        }
        mcp_put_hex(&t, v);
        mcp_put(&t, " ");
        describe(&t, g, v);
        mcp_put(&t, "\n");
    }
    return t.p;
}

/* each instruction: its address, its text, and the address a branch names */
typedef struct {
    Text* t;
    const Game* g;
} Listing;
static void listed(void* ctx, SnxU64 address, SnxU32 word, const char* text, SnxU64 target) {
    Listing* l = (Listing*)ctx;
    char w[9];
    if (l->t->lines >= MAX_LINES) return;
    sprintf(w, "%08lx", (unsigned long)word);
    mcp_put(l->t, "  ");
    mcp_put_hex(l->t, address);
    mcp_put(l->t, ":  ");
    mcp_put(l->t, w);
    mcp_put(l->t, "  ");
    mcp_put(l->t, text);
    if (target) {
        mcp_put(l->t, "  ; ");
        mcp_put_hex(l->t, target);
        mcp_put(l->t, " ");
        describe(l->t, l->g, target);
    }
    mcp_put(l->t, "\n");
    l->t->lines++;
}

/* a function by a part of its name (demangled or not), else the range start to end */
static char* disassemble(Mcp* m, const char* game, const char* function, const char* start, const char* end,
                         Text* err) {
    char* folder = game_folder(m, game, err);
    const SnxModuleImage* mi;
    const Game* g;
    Text t = {0};
    Listing l;
    SnxU64 from = 0, to = 0;
    size_t i, found = 0;
    if (!folder) return NULL;
    g = game_get(m, folder, err);
    free(folder);
    if (!g) return NULL;
    if (!g->program.npdm.is64) {
        mcp_put(err, "a 32-bit game: disassemble reads A64 only (tools/disasm.py reads A32 and Thumb)");
        goto done;
    }
    if (function) {
        const Sym* hit = NULL;
        for (i = 0; i < g->count; i++) {
            const char* name = sym_name(g, &g->syms[i]);
            char* plain = !strncmp(name, "_Z", 2) ? snx_demangle(name) : NULL;
            if (strstr(name, function) || (plain && strstr(plain, function))) {
                if (!found++) hit = &g->syms[i];
            }
            free(plain);
        }
        if (!hit) {
            mcp_put(err, "no symbol holds that name");
            goto done;
        }
        from = hit->address;
        to = g->program.modules[hit->module].symbols[hit->symbol].size
                 ? from + g->program.modules[hit->module].symbols[hit->symbol].size
                 : 0;
        if (!to) /* no size: up to the next symbol */
            for (i = (size_t)(hit - g->syms) + 1; i < g->count && !to; i++)
                if (g->syms[i].address > from) to = g->syms[i].address;
        if (!to || to - from > MAX_FUNCTION) to = from + MAX_FUNCTION;
        put_name(&t, sym_name(g, hit));
        mcp_put(&t, ":\n");
    } else if (!start || !end || !parse_hex(start, &from) || !parse_hex(end, &to) || to <= from) {
        mcp_put(err, "name a function, or a range: start and end, hexadecimal, end past start");
        goto done;
    }
    from &= ~(SnxU64)3;
    mi = module_at(g, from);
    if (!mi || from - mi->base >= mi->image_size) {
        mcp_put(err, "no module's code there");
        goto done;
    }
    if (to - mi->base > mi->image_size) to = mi->base + mi->image_size;
    l.t = &t;
    l.g = g;
    if (snx_disasm_a64(mi->image + (from - mi->base), (size_t)(to - from), from, listed, &l)) {
        mcp_put(err, "this build has no A64 disassembler (its LLVM has no AArch64 target): tools/disasm.py has one");
        goto done;
    }
    if (t.lines >= MAX_LINES) mcp_put(&t, "... (cut at 400 instructions: give a range for the rest)\n");
    if (found > 1) {
        mcp_put(&t, "(");
        mcp_put_num(&t, (unsigned long)found - 1);
        mcp_put(&t, " more symbols hold that name: a longer part of it picks one)\n");
    }
done:
    if (err->n) {
        free(t.p);
        return NULL;
    }
    return t.p ? t.p : snx_strdup("");
}

/* ============================================================ the protocol */
static const char instructions[] =
    "ScarletNX's logs and games on this machine. scan_log summarizes a game's log (how the run ended, where, the "
    "services the runtime doesn't implement, the shaders, macros and SVCs it missed, slow frames); read_log reads parts of it by text or line; find_game "
    "lists the extracted games; symbolize and disassemble read a game's code at the addresses a log names. "
    "A log's addresses match only the same game version (find_game gives it, the log's header names the log's). "
    "For a game started with the GDB stub (-S, or -s to wait before the runtime starts), gdb_attach connects GDB; "
    "gdb_command, gdb_threads, gdb_guest and gdb_memory then read the runtime and the guest, and gdb_detach lets the "
    "game run on. "
    "Nothing of a game (code, files) goes into an issue, a pull request or an edenprobe case: a case asks the "
    "hardware a question the failure narrowed down to.";

static cJSON* schema(const char* const* names, const char* const* types, int n, const char* const* required, int r) {
    cJSON *s = cJSON_CreateObject(), *props = cJSON_CreateObject(), *req = cJSON_CreateArray();
    int i;
    cJSON_AddStringToObject(s, "type", "object");
    for (i = 0; i < n; i++) {
        cJSON* p = cJSON_CreateObject();
        if (!strcmp(types[i], "string[]")) {
            cJSON* items = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "type", "array");
            cJSON_AddStringToObject(items, "type", "string");
            cJSON_AddItemToObject(p, "items", items);
        } else
            cJSON_AddStringToObject(p, "type", types[i]);
        cJSON_AddItemToObject(props, names[i], p);
    }
    cJSON_AddItemToObject(s, "properties", props);
    for (i = 0; i < r; i++) cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
    cJSON_AddItemToObject(s, "required", req);
    cJSON_AddBoolToObject(s, "additionalProperties", 0);
    return s;
}
static void tool(cJSON* list, const char* name, const char* description, cJSON* input) {
    cJSON* t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", description);
    cJSON_AddItemToObject(t, "inputSchema", input);
    cJSON_AddItemToArray(list, t);
}
static cJSON* tools(void) {
    static const char* const path[] = {"path"};
    static const char* const s1[] = {"string"};
    static const char* const read_names[] = {"path", "pattern", "ignore_case", "context", "start", "count"};
    static const char* const read_types[] = {"string", "string", "boolean", "integer", "integer", "integer"};
    static const char* const find_names[] = {"title_id"};
    static const char* const sym_names[] = {"game", "addresses"};
    static const char* const sym_types[] = {"string", "string[]"};
    static const char* const dis_names[] = {"game", "function", "start", "end"};
    static const char* const dis_types[] = {"string", "string", "string", "string"};
    static const char* const game[] = {"game"};
    static const char scan_says[] =
        "The summary of a game's .log or the launcher's Share Log file: build, title ID, game version, system, GPU, how the "
        "run ended (crash, svcBreak, exception, BRK, an instruction the recompiler cannot run, fatal), the game "
        "function it is pinned on with its backtrace, "
        "the service commands the runtime doesn't implement, what else it missed (shaders not translated, GPU classes "
        "and operations, macros with no native or compiled form, SVCs, guest code the JIT compiled), warnings, the "
        "frames over 50 ms and what the slowest did, "
        "and a signature shared by every log that failed the same way.";
    static const char read_says[] =
        "Parts of a log with line numbers, never the whole of a large one: the lines holding pattern (plain text, "
        "alternatives separated by |, ignore_case to match any case) with context lines around each (0 to 20), "
        "or count lines from start (1-based), or the last count lines. 400 lines at most. Reads files under "
        "ScarletNX's data folder and its state, cache and settings folders.";
    static const char find_says[] =
        "The extracted games on this machine (the data folder's work/) with their title IDs and versions, or only "
        "title_id's. A log's addresses match only a game of the version its header names.";
    static const char states_says[] =
        "A recompiled game's save states (the data folder's games/<title_id>/states/): each one's file, program, slot, "
        "when it was saved (seconds since 1970), the game's play time then, the SNX_ABI, runtime and update and mods "
        "stamp that saved it, and why this build can't load it by its header (cannot_load, null when it can: a state whose "
        "contents have another layout is only refused when it is loaded).";
    static const char sym_says[] =
        "Names game addresses (a backtrace's, a crash report's, hexadecimal) by the game's symbols: module, function "
        "and offset. game is a folder, a game folder of work/, or a title ID.";
    static const char dis_says[] =
        "Disassembles a game function named by a part of its symbol (demangled or not), or the range start to end "
        "(hexadecimal), with each branch's target named. 400 instructions at most. game as for symbolize.";
    static const char* const scaffold_names[] = {"checkout", "suite", "id", "name", "tier", "issue", "family"};
    static const char* const scaffold_types[] = {"string", "string", "string", "string", "string", "integer", "string"};
    static const char* const check_names[] = {"checkout", "suite"};
    static const char* const s2[] = {"string", "string"};
    static const char* const run_names[] = {"nro", "mode", "filter"};
    static const char* const s3[] = {"string", "string", "string"};
    static const char* const finding_names[] = {"issue", "suite", "name", "scarletnx_report", "reference_report",
                                                "source"};
    static const char* const finding_types[] = {"integer", "string", "string", "string", "string", "string"};
    static const char scaffold_says[] =
        "For a confirmed game report (the `status: confirmed` label): writes a new edenprobe service case's "
        "mechanical parts into a local edenprobe checkout (git clone of scarletnx/edenprobe), every file or none: "
        "the case's enum entry and CASES[] row (suite, name), a judge and its reference check to fill in, the device "
        "side asking nothing yet, and a new suite's ui.c row. family is service (the only one it writes yet); id is "
        "the enum name without SERVICE_ (HID_HOLD_TYPE); "
        "name is one sentence about the hardware's behaviour; tier is none, exact, ulp, position or level; issue is "
        "the ScarletNX issue. It answers what is left to write: the service call, the judge, the expected answer's "
        "source, the docs. Nothing of the game goes into a case.";
    static const char check_says[] =
        "Runs edenprobe's host check in a checkout (make -f host.mk host-run: every judge and its reference check, "
        "with the host's compiler, no Switch toolchain): its lines about suite, its failures and its end.";
    static const char run_says[] =
        "Recompiles an edenprobe NRO (built from the checkout with devkitPro's image, or a pull request's CI "
        "artifact) with this program, runs it headless in the mode's words (\"quick notime service\", plus \"fatal\" "
        "for a case that may crash) until its summary, "
        "relaunching after a crash, and answers the report's build, the cases filter names (\"suite\" or \"suite: "
        "name\") and its summary. The recompile is kept while the NRO is unchanged; nothing of the person's "
        "ScarletNX folders is touched. The report stays in the cache for edenprobe_finding.";
    static const char finding_says[] =
        "The case's lines in ScarletNX's report.jsonl and the reference's (Eden's run, or a console's), checked "
        "(it should fail on ScarletNX and pass on the reference), and the edenprobe pull request's text: "
        "the commit subject, the issue it comes from, what each machine answered, source (where the expected answer "
        "comes from), the tests, and what a case may not hold.";
    static const char* const attach_names[] = {"port", "game"};
    static const char* const attach_types[] = {"integer", "string"};
    static const char* const command_names[] = {"command", "wait"};
    static const char* const command_types[] = {"string", "integer"};
    static const char* const guest_names[] = {"thread", "frames"};
    static const char* const guest_types[] = {"integer", "integer"};
    static const char* const memory_names[] = {"address", "size"};
    static const char* const memory_types[] = {"string", "integer"};
    static const char attach_says[] =
        "Connects GDB to a game started with the GDB stub (-S: it runs until a debugger connects, -s: it waits "
        "before the runtime starts), on the stub's port (1234). The game stops; a breakpoint on the runtime's "
        "crash_report stops it at a crash, and SIGSEGV and SIGBUS pass to the runtime (memory it moves, its crash "
        "report). game (a folder, a game of work/ or a title ID, of the same version) names guest addresses and the "
        "recompiled code's symbols (F_<address> a 64-bit game's functions, A_ and T_<address> a 32-bit game's A32 and "
        "Thumb blocks, a32_<address>_<thumb> the 32-bit JIT's) by the game's own symbols. GDB debugs the host "
        "process: the runtime's C with its debug information, and the recompiled game code. The stub takes one "
        "connection per run, so a second attach is refused. Needs gdb on PATH (GDB 9 or later). Linux.";
    static const char command_says[] =
        "Runs a GDB command and answers its output, 400 lines at most. Only reading, breakpoint and execution commands "
        "run (backtrace, info, print, x, ptype, list, disassemble, frame, up, down, thread, break, tbreak, watch, "
        "delete, disable, enable, condition, display, show, set var, set print, maintenance info, and the resumes), "
        "by GDB's names, aliases and unambiguous prefixes: no shell, script, file write, command body (commands, "
        "define, if, while) or session change (gdb_attach and gdb_detach do that). print, call and the like evaluate "
        "the game's own functions, which can do anything the game can. A resume (continue, next, step, stepi, nexti, "
        "finish, until, advance, jump) runs in the background and waits up to wait seconds (10, at most 600) for the "
        "game to stop, at a breakpoint or a crash, then interrupts it; wait 0 leaves it running, and nothing else runs "
        "until \"interrupt\" stops it. Example: p *(KThread*)t_hle.thread.cur, the selected thread's guest thread.";
    static const char threads_says[] =
        "Every thread of the stopped game: the runtime's own, and each guest thread with its id, name (the SDK's), "
        "whether it is in an SVC (its registers then exact; in game code they may be stale), its guest pc and lr, "
        "named with gdb_attach's game, and where its host thread is. Each line starts with the thread's GDB number.";
    static const char guest_says[] =
        "A guest thread's registers (X0-X30, SP, PC, NZCV and TPIDRRO_EL0, or a 32-bit game's R0-R15 and CPSR) and "
        "its guest backtrace from the frame records its frame pointer leads to, named with gdb_attach's game. thread "
        "is the GDB thread number gdb_threads lists (the selected thread when left out); frames (32, at most 64).";
    static const char memory_says[] =
        "Guest memory at address (hexadecimal, a game address within the game's address space), size bytes (256, at "
        "most 4096), as hex and text. The game must be stopped. A page the GPU holds can't be read: it is trapped "
        "until the game's CPU touches it.";
    static const char detach_says[] = "Detaches GDB, and the game runs on without it. Attaching again needs a new run.";
    cJSON* list = cJSON_CreateArray();
    tool(list, "scan_log", scan_says, schema(path, s1, 1, path, 1));
    tool(list, "read_log", read_says, schema(read_names, read_types, 6, path, 1));
    tool(list, "find_game", find_says, schema(find_names, s1, 1, NULL, 0));
    tool(list, "state_info", states_says, schema(find_names, s1, 1, find_names, 1));
    tool(list, "symbolize", sym_says, schema(sym_names, sym_types, 2, sym_names, 2));
    tool(list, "disassemble", dis_says, schema(dis_names, dis_types, 4, game, 1));
    tool(list, "edenprobe_scaffold", scaffold_says, schema(scaffold_names, scaffold_types, 7, scaffold_names, 7));
    tool(list, "edenprobe_check", check_says, schema(check_names, s2, 2, check_names, 1));
    tool(list, "edenprobe_run", run_says, schema(run_names, s3, 3, run_names, 3));
    tool(list, "edenprobe_finding", finding_says, schema(finding_names, finding_types, 6, finding_names, 4));
    tool(list, "gdb_attach", attach_says, schema(attach_names, attach_types, 2, NULL, 0));
    tool(list, "gdb_command", command_says, schema(command_names, command_types, 2, command_names, 1));
    tool(list, "gdb_threads", threads_says, schema(NULL, NULL, 0, NULL, 0));
    tool(list, "gdb_guest", guest_says, schema(guest_names, guest_types, 2, NULL, 0));
    tool(list, "gdb_memory", memory_says, schema(memory_names, memory_types, 2, memory_names, 1));
    tool(list, "gdb_detach", detach_says, schema(NULL, NULL, 0, NULL, 0));
    return list;
}

static const char* arg_string(const cJSON* args, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
/* otherwise too for a number a long can't hold (converting it would be undefined), NaN included */
static long arg_number(const cJSON* args, const char* key, long otherwise) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsNumber(v) && v->valuedouble >= (double)LONG_MIN && v->valuedouble <= (double)LONG_MAX
               ? (long)v->valuedouble
               : otherwise;
}

/* a guest address named by gdb_attach's game (mcp_int.h's McpNamer) */
static void name_guest(Text* t, const void* ctx, SnxU64 a) { describe(t, (const Game*)ctx, a); }
/* the GDB tools' namer: gdb_attach's game's, loaded as symbolize loads it, or none */
static McpNamer gdb_namer(Mcp* m, const void** ctx) {
    Text err = {0};
    const Game* g = m->gdb_game ? game_get(m, m->gdb_game, &err) : NULL;
    free(err.p);
    *ctx = g;
    return g ? name_guest : NULL;
}
static char* gdb_tool(Mcp* m, const char* name, const cJSON* args, Text* err) {
    const void* ctx;
    McpNamer namer;
    if (!strcmp(name, "gdb_attach")) {
        const char* game = arg_string(args, "game");
        char* folder = game ? game_folder(m, game, err) : NULL;
        char* text = NULL;
        if (game && !folder) return NULL;
        if (folder && !game_get(m, folder, err)) { /* loaded now: names that won't come should say why at once */
            free(folder);
            return NULL;
        }
        text = mcp_gdb_attach(arg_number(args, "port", 1234), err);
        if (text) {
            free(m->gdb_game);
            m->gdb_game = folder;
            folder = NULL;
        }
        free(folder);
        return text;
    }
    /* the game's symbols only for the tools that name: loading them evicts symbolize's game */
    ctx = NULL;
    namer = NULL;
    if (!strcmp(name, "gdb_command") || !strcmp(name, "gdb_threads") || !strcmp(name, "gdb_guest"))
        namer = gdb_namer(m, &ctx);
    if (!strcmp(name, "gdb_command")) {
        const char* command = arg_string(args, "command");
        if (command) return mcp_gdb_command(command, arg_number(args, "wait", 10), namer, ctx, err);
        mcp_put(err, "command: a GDB command");
    } else if (!strcmp(name, "gdb_threads"))
        return mcp_gdb_threads(namer, ctx, err);
    else if (!strcmp(name, "gdb_guest")) {
        long frames = arg_number(args, "frames", 32);
        if (frames < 0 || frames > 64) frames = 64; /* in range before the int */
        return mcp_gdb_guest(arg_number(args, "thread", 0), (int)frames, namer, ctx, err);
    } else if (!strcmp(name, "gdb_memory")) {
        const char* address = arg_string(args, "address");
        SnxU64 a;
        if (address && parse_hex(address, &a)) return mcp_gdb_memory(a, arg_number(args, "size", 256), err);
        mcp_put(err, "address: a hexadecimal game address");
    } else if (!strcmp(name, "gdb_detach")) {
        free(m->gdb_game);
        m->gdb_game = NULL;
        return mcp_gdb_detach(err);
    } else {
        mcp_put(err, "no tool ");
        mcp_put(err, name);
    }
    return NULL;
}

/* the tool's answer as an MCP result: its text, or the error's with isError */
static cJSON* call(Mcp* m, const char* name, const cJSON* args) {
    cJSON *result = cJSON_CreateObject(), *content = cJSON_CreateArray(), *item = cJSON_CreateObject();
    Text err = {0};
    char* text = NULL;
    const char* path = arg_string(args, "path");
    if (!strcmp(name, "scan_log")) {
        if (!path)
            mcp_put(&err, "path: the log");
        else if (!readable(m, path))
            mcp_put(&err, outside);
        else
            text = scan_log(path, &err);
    } else if (!strcmp(name, "read_log")) {
        if (!path)
            mcp_put(&err, "path: the log");
        else if (!readable(m, path))
            mcp_put(&err, outside);
        else
            text = read_log(path, arg_string(args, "pattern"),
                            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "ignore_case")),
                            (int)arg_number(args, "context", 2), arg_number(args, "start", 0),
                            (int)arg_number(args, "count", 100), &err);
    } else if (!strcmp(name, "find_game")) {
        const char* id = arg_string(args, "title_id");
        SnxU64 tid = 0;
        if (id && !parse_hex(id, &tid))
            mcp_put(&err, "title_id: sixteen hexadecimal digits");
        else {
            cJSON* list = cJSON_CreateArray();
            find_games(m, tid, list);
            text = cJSON_Print(list);
            cJSON_Delete(list);
        }
    } else if (!strcmp(name, "state_info")) {
        const char* id = arg_string(args, "title_id");
        SnxU64 tid = 0;
        if (!id || !parse_hex(id, &tid) || !tid)
            mcp_put(&err, "title_id: sixteen hexadecimal digits");
        else {
            cJSON* list = cJSON_CreateArray();
            find_states(m, tid, list);
            text = cJSON_Print(list);
            cJSON_Delete(list);
        }
    } else if (!strcmp(name, "symbolize") || !strcmp(name, "disassemble")) {
        const char* game = arg_string(args, "game");
        if (!game)
            mcp_put(&err, "game: a folder, a game of work/ or a title ID");
        else if (name[0] == 's')
            text = symbolize(m, game, cJSON_GetObjectItemCaseSensitive(args, "addresses"), &err);
        else
            text = disassemble(m, game, arg_string(args, "function"), arg_string(args, "start"),
                               arg_string(args, "end"), &err);
    } else if (!strcmp(name, "edenprobe_scaffold")) {
        const char *checkout = arg_string(args, "checkout"), *suite = arg_string(args, "suite"),
                   *id = arg_string(args, "id"), *what = arg_string(args, "name"), *family = arg_string(args, "family");
        if (!checkout || !suite || !id || !what || !family)
            mcp_put(&err, "checkout, family, suite, id, name, tier and issue");
        else if (strcmp(family, "service"))
            mcp_put(&err, "family: the scaffold writes service cases only yet; a cpu case goes in "
                          "source/tests/t_cpu.c by hand (a name, an inline-asm helper, CHECK_TRUE)");
        else
            text = mcp_edenprobe_scaffold(checkout, suite, id, what, arg_string(args, "tier"),
                                          arg_number(args, "issue", 0), &err);
    } else if (!strcmp(name, "edenprobe_check")) {
        const char* checkout = arg_string(args, "checkout");
        if (!checkout)
            mcp_put(&err, "checkout: the edenprobe checkout");
        else
            text = mcp_edenprobe_check(checkout, arg_string(args, "suite"), &err);
    } else if (!strcmp(name, "edenprobe_run")) {
        const char *nro = arg_string(args, "nro"), *mode = arg_string(args, "mode"),
                   *filter = arg_string(args, "filter");
        if (!nro || !mode || !filter)
            mcp_put(&err, "nro, mode and filter");
        else if (!m->self || !m->cache)
            mcp_put(&err, "this program's path or its cache folder is unknown");
        else
            text = mcp_edenprobe_run(m->self, m->cache, nro, mode, filter, &err);
    } else if (!strcmp(name, "edenprobe_finding")) {
        text = mcp_edenprobe_finding(arg_number(args, "issue", 0), arg_string(args, "suite"), arg_string(args, "name"),
                                     arg_string(args, "scarletnx_report"), arg_string(args, "reference_report"),
                                     arg_string(args, "source"), &err);
    } else if (!strncmp(name, "gdb_", 4)) {
        text = gdb_tool(m, name, args, &err);
    } else {
        mcp_put(&err, "no tool ");
        mcp_put(&err, name);
    }
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : err.p ? err.p : "failed");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    if (!text) cJSON_AddBoolToObject(result, "isError", 1);
    free(text);
    free(err.p);
    return result;
}

/* the reply to one message, or NULL for a notification */
static cJSON* handle(Mcp* m, const cJSON* message) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "id"), *params;
    const char* method = arg_string(message, "method");
    cJSON *reply, *result = NULL;
    if (!id || !method) return NULL;
    params = cJSON_GetObjectItemCaseSensitive(message, "params");
    if (!strcmp(method, "initialize")) {
        const char* version = params ? arg_string(params, "protocolVersion") : NULL;
        cJSON *caps = cJSON_CreateObject(), *info = cJSON_CreateObject();
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", version ? version : "2025-06-18");
        cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
        cJSON_AddItemToObject(result, "capabilities", caps);
        cJSON_AddStringToObject(info, "name", "scarletnx");
        cJSON_AddStringToObject(info, "version", "1");
        cJSON_AddItemToObject(result, "serverInfo", info);
        cJSON_AddStringToObject(result, "instructions", instructions);
    } else if (!strcmp(method, "tools/list")) {
        result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "tools", tools());
    } else if (!strcmp(method, "tools/call")) {
        const char* name = params ? arg_string(params, "name") : NULL;
        const cJSON* args = params ? cJSON_GetObjectItemCaseSensitive(params, "arguments") : NULL;
        result = name ? call(m, name, args) : NULL;
    } else if (!strcmp(method, "ping"))
        result = cJSON_CreateObject();
    reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
    cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(id, 1));
    if (result)
        cJSON_AddItemToObject(reply, "result", result);
    else {
        cJSON* error = cJSON_CreateObject();
        cJSON_AddNumberToObject(error, "code", -32601);
        cJSON_AddStringToObject(error, "message", "no such method, or a tools/call without a name");
        cJSON_AddItemToObject(reply, "error", error);
    }
    return reply;
}

static void send(cJSON* reply) {
    char* text = cJSON_PrintUnformatted(reply);
    if (text) {
        fputs(text, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(text);
    }
    cJSON_Delete(reply);
}

int snx_mcp(int argc, char** argv) {
    Mcp m;
    Text line = {0}, pending = {0};
    if (mcp_open(&m, argc, argv)) return 1;
    while (mcp_read_line(&pending, &line)) {
        if (line.n && strspn(line.p, " \t\r\n") != line.n) {
            cJSON* message = cJSON_Parse(line.p);
            if (!message) {
                cJSON *reply = cJSON_CreateObject(), *error = cJSON_CreateObject();
                cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
                cJSON_AddNullToObject(reply, "id");
                cJSON_AddNumberToObject(error, "code", -32700);
                cJSON_AddStringToObject(error, "message", "not JSON");
                cJSON_AddItemToObject(reply, "error", error);
                send(reply);
            } else {
                cJSON* reply = handle(&m, message);
                if (reply) send(reply);
                cJSON_Delete(message);
            }
        }
        line.n = 0;
        if (line.p) line.p[0] = 0;
    }
    free(line.p);
    free(pending.p);
    mcp_close(&m);
    return 0;
}
