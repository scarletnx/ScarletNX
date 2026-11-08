/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx --probe (probe.h): what scripts/edenprobe.py's run does for CI, on a player's machine. The run's data folder
   is a portable one (SNX_PORTABLE_DIR) inside the probe's folder, so the game made there finds its users, saves,
   settings, logs and caches there and none of the player's; the GPU driver package a run loads is still the player's
   (gpu_drivers/ of the user's data folder). Each run starts edenprobe's card over but for its fs files, written once:
   no report, journal or crash list of an earlier run, so a case reported as crashed ended this run's program, with
   the driver the machine has now. A launch that ends before the summary is followed by another, which edenprobe's
   journal goes on from after the case the last one ended inside (reported as crashed) */
#include "nx/runtime/driver/probe.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include <openssl/evp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#ifdef _WIN32
#define PROGRAM_SUFFIX ".exe"
#else
#define PROGRAM_SUFFIX ""
#endif
#define NAME "edenprobe"

/* ============================================================ text */
typedef struct {
    char* p;
    size_t n;
} Text;
static void put(Text* t, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Text* t, const char* fmt, ...) {
    va_list a, b;
    int k;
    va_start(a, fmt);
    va_copy(b, a);
    k = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    if (k > 0) {
        t->p = (char*)snx_realloc(t->p, t->n + (size_t)k + 1);
        vsnprintf(t->p + t->n, (size_t)k + 1, fmt, b);
        t->n += (size_t)k;
    }
    va_end(b);
}
/* 1 when the n bytes at s hold lit */
static int holds(const char* s, size_t n, const char* lit) {
    size_t k = strlen(lit), i;
    for (i = 0; k <= n && i <= n - k; i++)
        if (!memcmp(s + i, lit, k)) return 1;
    return 0;
}
/* the file's text with a NUL after it, NULL when it cannot be read; its size into *size when size is given */
static char* text_of(const char* path, size_t* size) {
    size_t n = 0;
    unsigned char* data = snx_is_file(path) ? snx_read_file(path, &n) : NULL;
    char* text;
    if (!data && !(snx_is_file(path) && snx_file_size(path) == 0)) return NULL;
    text = (char*)snx_alloc(n + 1);
    if (n) memcpy(text, data, n);
    text[n] = 0;
    free(data);
    if (size) *size = n;
    return text;
}

/* ============================================================ the report while it is written */
/* the report the latest launch writes, in the folder sdmc:/edenprobe/last-run names ("runs/2a3ec1e"); NULL for none */
static char* report_of(const char* sdmc) {
    char *last = snx_path_join(sdmc, "last-run"), *name = text_of(last, NULL), *folder, *path = NULL;
    size_t n;
    free(last);
    if (!name) return NULL;
    for (n = strlen(name); n && (name[n - 1] == '\n' || name[n - 1] == '\r' || name[n - 1] == ' '); n--)
        name[n - 1] = 0;
    if (name[0]) {
        folder = snx_path_join(sdmc, name);
        path = snx_path_join(folder, "report.jsonl");
        free(folder);
        if (!snx_is_file(path)) {
            free(path);
            path = NULL;
        }
    }
    free(name);
    return path;
}
/* the case lines of a report, whole lines only: the last one may be half written */
static int case_lines(const char* text, size_t n) {
    const char *line = text, *end = text + n, *next;
    int cases = 0;
    while (line < end && (next = (const char*)memchr(line, '\n', (size_t)(end - line)))) {
        if (holds(line, (size_t)(next - line), "\"type\":\"case\"") ||
            holds(line, (size_t)(next - line), "\"type\": \"case\""))
            cases++;
        line = next + 1;
    }
    return cases;
}
/* the case the journal beside the report says the program was inside: its last record a case's start ("start <suite>
   <pass> <case>", ended by a tab). 1 with the suite and the case, else 0 */
static int inside(const char* report, char* suite, size_t suite_size, char* name, size_t name_size) {
    char *folder = snx_dirname(report), *path = snx_path_join(folder, "journal"), *text = text_of(path, NULL);
    char *end, *line, *tab;
    int found = 0;
    free(folder);
    free(path);
    if (!text) return 0;
    end = text + strlen(text);
    if (end > text && end[-1] == '\n') *--end = 0;
    /* a record ends in its tab: a last line without one is a write cut short */
    if (end > text && end[-1] == '\t') {
        end[-1] = 0;
        line = strrchr(text, '\n');
        line = line ? line + 1 : text;
        if (!strncmp(line, "start ", 6)) {
            char* at = line + 6;
            size_t k = strcspn(at, " ");
            tab = at + k;
            if (*tab == ' ' && k < suite_size) {
                memcpy(suite, at, k);
                suite[k] = 0;
                at = tab + 1 + strspn(tab + 1, "0123456789"); /* the pass */
                if (*at == ' ' && at[1]) {
                    snprintf(name, name_size, "%s", at + 1);
                    found = 1;
                }
            }
        }
    }
    free(text);
    return found;
}
static const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return " (SIGSEGV)";
        case SIGABRT: return " (SIGABRT)";
        case SIGILL: return " (SIGILL)";
        case SIGFPE: return " (SIGFPE)";
        case SIGTERM: return " (SIGTERM)";
#ifdef SIGBUS
        case SIGBUS: return " (SIGBUS)";
#endif
#ifdef SIGKILL
        case SIGKILL: return " (SIGKILL)";
#endif
#ifdef SIGTRAP
        case SIGTRAP: return " (SIGTRAP)";
#endif
        default: return "";
    }
}
/* how a launch ended, after "launch N " */
static void ended(const SnxChild* c, int killed, double quiet, char* out, size_t size) {
    if (killed)
        snprintf(out, size, "was ended after %.0f s with no new line in the report", quiet);
    else if (c->signal)
        snprintf(out, size, "ended by signal %d%s", c->signal, signal_name(c->signal));
    else if (c->code > 255) /* Windows: an exception's code */
        snprintf(out, size, "ended with exit 0x%lx", c->code);
    else
        snprintf(out, size, "ended with exit %lu", c->code);
}

/* ============================================================ the report once it is done */
typedef struct {
    char name[64];
    int failed, crashed;
} Suite;
typedef struct {
    char version[32], build[64]; /* the first note's: "edenprobe 0.9.0 (v0.9.0) starting" */
    long passed, failed, skipped, crashed;
    int summary, cases, broken;
    char* ignored; /* the mode note's words edenprobe does not know: "1 unknown word ignored: display" */
    Suite* suites;
    size_t count;
} Result;
static Suite* suite_of(Result* r, const char* name) {
    size_t i;
    for (i = 0; i < r->count; i++)
        if (!strcmp(r->suites[i].name, name)) return &r->suites[i];
    r->suites = (Suite*)snx_realloc(r->suites, (r->count + 1) * sizeof *r->suites);
    memset(&r->suites[r->count], 0, sizeof *r->suites);
    snprintf(r->suites[r->count].name, sizeof r->suites[r->count].name, "%s", name);
    return &r->suites[r->count++];
}
static const char* str_of(yyjson_val* o, const char* key) { return yyjson_get_str(yyjson_obj_get(o, key)); }
static void note(Result* r, const char* text) {
    const char* at;
    char version[32], build[64];
    if (!r->version[0] && strstr(text, ") starting") && sscanf(text, "edenprobe %31s (%63[^)])", version, build) == 2) {
        memcpy(r->version, version, sizeof version);
        memcpy(r->build, build, sizeof build);
    }
    at = !strncmp(text, "mode:", 5) ? strstr(text, " unknown word") : NULL;
    if (at && !r->ignored) {
        while (at > text && at[-1] >= '0' && at[-1] <= '9') at--;
        r->ignored = snx_strdup(at);
    }
}
static void read_result(const char* text, Result* r) {
    const char* line = text;
    while (line && *line) {
        const char* end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        yyjson_doc* doc = n ? yyjson_read(line, n, 0) : NULL;
        yyjson_val* o = doc ? yyjson_doc_get_root(doc) : NULL;
        const char* type = o ? str_of(o, "type") : NULL;
        if (n && !type) r->broken++;
        if (type && !strcmp(type, "note") && str_of(o, "text")) note(r, str_of(o, "text"));
        if (type && !strcmp(type, "case") && str_of(o, "suite") && str_of(o, "result")) {
            const char *result = str_of(o, "result"), *reason = str_of(o, "reason");
            r->cases++;
            if (!strcmp(result, "fail")) suite_of(r, str_of(o, "suite"))->failed++;
            if (!strcmp(result, "skip") && reason && !strcmp(reason, "crashed"))
                suite_of(r, str_of(o, "suite"))->crashed++;
        }
        if (type && !strcmp(type, "summary")) {
            r->summary = 1;
            r->passed = (long)yyjson_get_sint(yyjson_obj_get(o, "passed"));
            r->failed = (long)yyjson_get_sint(yyjson_obj_get(o, "failed"));
            r->skipped = (long)yyjson_get_sint(yyjson_obj_get(o, "skipped"));
            r->crashed = (long)yyjson_get_sint(yyjson_obj_get(o, "crashed"));
        }
        yyjson_doc_free(doc);
        line = end ? end + 1 : NULL;
    }
}
static int by_failed(const void* a, const void* b) {
    const Suite *x = (const Suite*)a, *y = (const Suite*)b;
    return x->failed != y->failed ? y->failed - x->failed : strcmp(x->name, y->name);
}
static int by_crashed(const void* a, const void* b) {
    const Suite *x = (const Suite*)a, *y = (const Suite*)b;
    return x->crashed != y->crashed ? y->crashed - x->crashed : strcmp(x->name, y->name);
}
/* the suites with failures (crashed 0) or crashes (1): " (astc 15, astcimage 9, tex8k 2)", the most first */
static void suites(Text* t, const Result* r, int crashed) {
    Suite* sorted;
    size_t i, listed = 0;
    if (!r->count) return;
    sorted = (Suite*)snx_alloc(r->count * sizeof *sorted);
    memcpy(sorted, r->suites, r->count * sizeof *sorted);
    qsort(sorted, r->count, sizeof *sorted, crashed ? by_crashed : by_failed);
    for (i = 0; i < r->count; i++) {
        int n = crashed ? sorted[i].crashed : sorted[i].failed;
        if (n) put(t, "%s%s %d", listed++ ? ", " : " (", sorted[i].name, n);
    }
    if (listed) put(t, ")");
    free(sorted);
}

/* ============================================================ the run */
static int sha256_of(const char* path, char hex[65]) {
    size_t size = 0, i;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    char* data = text_of(path, &size);
    int ok;
    if (!data) return -1;
    ok = EVP_Digest(data, size, digest, &n, EVP_sha256(), NULL) && n == 32;
    free(data);
    if (!ok) return -1;
    for (i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    return 0;
}
/* the file at path gone when it is there: 0, or -1 said */
static int drop(const char* path) {
    char why[256];
    if (!snx_path_exists(path)) return 0;
    if (snx_is_dir(path) ? !snx_remove_tree(path) : !remove(path)) return 0;
    snx_error("%s cannot be removed: %s", path, snx_os_error(why, sizeof why));
    return -1;
}
/* the program at argv[0] run to its end, its output into log, how it ended in c: 0, or -1 said */
static int run_to_end(const char* const* argv, const char* cwd, const char* log, SnxChild* c) {
    char why[1024];
    int waited;
    if (snx_child_start(c, argv, cwd, log, why, sizeof why)) {
        snx_error("%s", why);
        return -1;
    }
    while (!(waited = snx_child_wait(c, 1000))) {}
    if (waited < 0) {
        snx_error("whether %s still runs cannot be told: %s", argv[0], snx_os_error(why, sizeof why));
        snx_child_kill(c);
    }
    snx_child_close(c);
    return waited < 0 ? -1 : 0;
}

int snx_probe_run(const SnxProbe* p) {
    char sha[65], why[1024], suite[64], name[512], how[160];
    char *data = snx_path_join(p->folder, SNX_PORTABLE_DIR), *recompiled = snx_path_join(data, "recompiled"),
         *game_dir = snx_path_join(recompiled, NAME), *game = snx_format("%s/%s%s", game_dir, NAME, PROGRAM_SUFFIX),
         *sdmc = snx_format("%s/%s_data/sdmc/%s", game_dir, NAME, NAME),
         *stamp_path = snx_path_join(p->folder, "stamp.txt"), *total_path = snx_path_join(p->folder, "total.txt"),
         *report_copy = snx_path_join(p->folder, "report.jsonl"),
         *summary_path = snx_path_join(p->folder, "summary.txt"),
         *recompile_log = snx_path_join(p->folder, "recompile.log"), *stamp = NULL, *old = NULL, *report = NULL,
         *text = NULL, *log = NULL;
    const char* const none[] = {NULL};
    const char* const* settings = p->settings ? p->settings : none;
    const char** argv = NULL;
    Text out = {0}, given = {0};
    Result r = {0};
    SnxChild c;
    int rc = 1, launch, before = -1, cases = 0, total = 0, count, i, killed = 0, launches = 0;
    size_t size = 0;
    snx_path_native(game);
    snx_path_native(sdmc);
    for (i = 0; settings[i]; i++) put(&given, "%s%s", i ? ", " : "", settings[i] + (settings[i][0] == '-' ? 2 : 0));
    if (snx_mkdirs(p->folder)) {
        snx_error("the probe's folder %s cannot be made: %s", p->folder, snx_os_error(why, sizeof why));
        goto done;
    }
    if (sha256_of(p->nro, sha)) {
        snx_error("%s cannot be read: %s", p->nro, snx_os_error(why, sizeof why));
        goto done;
    }
    fprintf(p->out, "probe: %s, sha256 %s, mode \"%s\", in %s\n", snx_path_leaf(p->nro), sha, p->mode, p->folder);
    fflush(p->out);
    /* what the last run left here goes: none of it is this run's */
    if (drop(report_copy) || drop(summary_path)) goto done;
    for (i = 1;; i++) {
        free(log);
        log = snx_format("%s/launch-%d.log", p->folder, i);
        snx_path_native(log);
        if (!snx_path_exists(log)) break;
        if (drop(log)) goto done;
    }

    /* the game, made again when the NRO or this program is another than the one it was made from */
    stamp = snx_format("nro %s\nprogram %s %llu %llu\n", sha, snx_build_commit(),
                       (unsigned long long)snx_file_size(p->self), (unsigned long long)snx_file_time(p->self));
    old = text_of(stamp_path, NULL);
    if (!old || strcmp(old, stamp) || !snx_is_file(game)) {
        const char* recompile[] = {p->self, "-r", p->nro, NAME, "--data", data, NULL};
        double began = snx_seconds();
        fprintf(p->out, "probe: recompiling %s\n", snx_path_leaf(p->nro));
        fflush(p->out);
        if (drop(stamp_path) || drop(game_dir)) goto done;
        if (snx_mkdirs(data)) {
            snx_error("%s cannot be made: %s", data, snx_os_error(why, sizeof why));
            goto done;
        }
        if (run_to_end(recompile, p->folder, recompile_log, &c)) goto done;
        if (c.signal || c.code || !snx_is_file(game)) {
            ended(&c, 0, 0, how, sizeof how);
            snx_error("the recompile %s%s%s: %s says why", how, snx_is_file(game) ? "" : " without making ",
                      snx_is_file(game) ? "" : game, recompile_log);
            goto done;
        }
        if (snx_write_file(stamp_path, stamp, strlen(stamp))) {
            snx_error("%s cannot be written: %s", stamp_path, snx_os_error(why, sizeof why));
            goto done;
        }
        fprintf(p->out, "probe: recompiled in %.0f s\n", snx_seconds() - began);
        fflush(p->out);
    }

    /* the card: the mode, and no earlier run's report, journal or crash list */
    {
        char *mode_path = snx_path_join(sdmc, "mode"), *runs = snx_path_join(sdmc, "runs"),
             *last = snx_path_join(sdmc, "last-run"), *crashes = snx_path_join(sdmc, "crashes"),
             *words = snx_format("%s\n", p->mode);
        int failed = drop(runs) || drop(last) || drop(crashes);
        if (!failed && (snx_mkdirs(sdmc) || snx_write_file(mode_path, words, strlen(words)))) {
            snx_error("the mode file %s cannot be written: %s", mode_path, snx_os_error(why, sizeof why));
            failed = 1;
        }
        free(mode_path);
        free(runs);
        free(last);
        free(crashes);
        free(words);
        if (failed) goto done;
    }
    /* the cases the last whole run of this NRO in this mode had, for "N of M cases" */
    free(old);
    old = text_of(total_path, NULL);
    if (old) {
        char held[65];
        int n = 0, at = 0;
        if (sscanf(old, "%64s %d %n", held, &n, &at) == 2 && !strcmp(held, sha) && at &&
            !strncmp(old + at, p->mode, strlen(p->mode)) && (old[at + strlen(p->mode)] == '\n'))
            total = n;
    }

    for (i = 0; settings[i]; i++) {}
    argv = (const char**)snx_alloc((size_t)(i + 3) * sizeof *argv);
    argv[0] = game;
    argv[1] = "-H";
    for (i = 0; settings[i]; i++) argv[2 + i] = settings[i];
    argv[2 + i] = NULL;
    for (launch = 1; launch <= p->launches; launch++) {
        double heard = snx_seconds(), said = 0;
        long seen = -1;
        int waited = 0, shown = -1;
        free(log);
        log = snx_format("%s/launch-%d.log", p->folder, launch);
        snx_path_native(log);
        killed = 0;
        if (snx_child_start(&c, argv, game_dir, log, why, sizeof why)) {
            snx_error("%s", why);
            goto done;
        }
        launches = launch;
        while (!c.ended) {
            waited = snx_child_wait(&c, 250);
            if (waited < 0) {
                snx_error("whether %s still runs cannot be told: %s", game, snx_os_error(why, sizeof why));
                snx_child_kill(&c);
                snx_child_close(&c);
                goto done;
            }
            free(report);
            report = report_of(sdmc);
            if (report && (long)snx_file_size(report) != seen) {
                seen = (long)snx_file_size(report);
                heard = snx_seconds();
                free(text);
                text = text_of(report, &size);
                cases = text ? case_lines(text, size) : 0;
            }
            /* the count each half second it changes, and the last one */
            if (cases != shown && (c.ended || snx_seconds() - said >= 0.5)) {
                if (total)
                    fprintf(p->out, "probe: %d of %d cases\n", cases, total);
                else
                    fprintf(p->out, "probe: %d case%s\n", cases, cases == 1 ? "" : "s");
                fflush(p->out);
                shown = cases;
                said = snx_seconds();
            }
            if (!c.ended && snx_seconds() - heard > p->quiet) {
                snx_child_kill(&c);
                killed = 1;
            }
        }
        snx_child_close(&c);
        ended(&c, killed, p->quiet, how, sizeof how);
        if (!report) {
            snx_error("launch %d %s and left no report: %s says why", launch, how, log);
            goto done;
        }
        count = text ? case_lines(text, size) : 0;
        if (text && (holds(text, size, "\"type\":\"summary\"") || holds(text, size, "\"type\": \"summary\""))) {
            if (killed || c.signal || c.code) put(&out, "probe: the program %s after its summary: %s\n", how, log);
            rc = 0;
            break;
        }
        if (count <= before) {
            snx_error("launch %d %s and added no case to the report: edenprobe would only end the same way again "
                      "(%s)",
                      launch, how, log);
            break;
        }
        before = count;
        if (launch == p->launches) {
            snx_error("%d launches and still no summary in the report: the last %s (%s)", launch, how, log);
            break;
        }
        if (inside(report, suite, sizeof suite, name, sizeof name))
            fprintf(p->out, "probe: launch %d %s inside %s case \"%s\"; launching again (%d of at most %d)\n", launch,
                    how, suite, name, launch + 1, p->launches);
        else
            fprintf(p->out, "probe: launch %d %s; launching again (%d of at most %d)\n", launch, how, launch + 1,
                    p->launches);
        fflush(p->out);
    }

    /* the result: the report's copy, its counts, and the suites that failed */
    if (report && snx_copy_file(report, report_copy))
        snx_error("%s cannot be copied to %s: %s", report, report_copy, snx_os_error(why, sizeof why));
    free(text);
    text = report ? text_of(report, &size) : NULL;
    if (text) read_result(text, &r);
    {
        Text head = {0};
        put(&head, "probe: edenprobe %s (%s), sha256 %s, mode \"%s\", %d launch%s, settings: %s\n",
            r.version[0] ? r.version : "?", r.build[0] ? r.build : "?", sha, p->mode, launches,
            launches == 1 ? "" : "es", given.p ? given.p : "defaults");
        if (r.summary) {
            put(&head, "probe: %ld passed, %ld failed", r.passed, r.failed);
            suites(&head, &r, 0);
            put(&head, ", %ld skipped, %ld crashed", r.skipped, r.crashed);
            suites(&head, &r, 1);
            put(&head, "\n");
        } else
            put(&head, "probe: no summary: %d case%s reported before the run ended\n", r.cases,
                r.cases == 1 ? "" : "s");
        if (r.ignored) put(&head, "probe: edenprobe did not know words of the mode: %s\n", r.ignored);
        if (r.broken) put(&head, "probe: %d lines of the report are no case, note or summary\n", r.broken);
        if (out.p) put(&head, "%s", out.p);
        if (report) put(&head, "probe: report %s\n", report_copy);
        fputs(head.p, p->out);
        fflush(p->out);
        if (snx_write_file(summary_path, head.p, head.n))
            snx_error("%s cannot be written: %s", summary_path, snx_os_error(why, sizeof why));
        free(head.p);
    }
    if (r.summary) {
        char* held = snx_format("%s %d %s\n", sha, r.cases, p->mode);
        if (snx_write_file(total_path, held, strlen(held)))
            snx_error("%s cannot be written: %s", total_path, snx_os_error(why, sizeof why));
        free(held);
    }
done:
    free(data);
    free(recompiled);
    free(game_dir);
    free(game);
    free(sdmc);
    free(stamp_path);
    free(total_path);
    free(report_copy);
    free(summary_path);
    free(recompile_log);
    free(stamp);
    free(old);
    free(report);
    free(text);
    free(log);
    free((void*)argv);
    free(out.p);
    free(given.p);
    free(r.ignored);
    free(r.suites);
    return rc;
}

/* ============================================================ scarletnx --probe */
int snx_probe_main(int argc, char** argv) {
#if defined(__ANDROID__) || defined(SNX_IOS)
    (void)argc;
    (void)argv;
    snx_error("--probe starts edenprobe as a program of its own, which an app's driver (Android, iOS) cannot: open "
              "edenprobe.nro as a game there");
    return 2;
#else
    const char *mode = NULL, *nro = NULL, **settings;
    char *self = NULL, *dir = NULL, *data = NULL, *state = NULL, *folder = NULL;
    int i, n = 0, rc = 2;
    SnxProbe probe;
    settings = (const char**)snx_alloc((size_t)argc * sizeof *settings);
    for (i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (!strncmp(arg, "--probe=", 8) || !strcmp(arg, "--probe")) {
            const char* value = arg[7] ? arg + 8 : "";
            if (mode) {
                snx_error("%s: --probe is given once", arg);
                goto done;
            }
            mode = !strcmp(value, "quick") ? SNX_PROBE_QUICK : !strcmp(value, "full") ? SNX_PROBE_FULL : NULL;
            if (!mode) {
                snx_error("--probe=%s: quick (cpu and graphics) or full (also fs, video, audio, kernel and display)",
                          value);
                goto done;
            }
        } else if (!strcmp(arg, "--probe-nro") || !strncmp(arg, "--probe-nro=", 12)) {
            nro = arg[11] ? arg + 12 : i + 1 < argc ? argv[++i] : NULL;
            if (!nro || !nro[0]) {
                snx_error("--probe-nro requires a file, the edenprobe.nro to run");
                goto done;
            }
        } else if (!strncmp(arg, "--gpu_driver=", 13) || !strncmp(arg, "--freedreno_env=", 16))
            settings[n++] = arg; /* the GPU driver the player picked: the one setting a probe takes */
        else {
            snx_error("unknown option with --probe: %s (it takes --probe-nro FILE, --gpu_driver=ID and "
                      "--freedreno_env=TEXT)",
                      arg);
            goto done;
        }
    }
    settings[n] = NULL;
    if (!mode) {
        snx_error("--probe-nro goes with --probe=quick or --probe=full");
        goto done;
    }
    if (!nro) {
        snx_error("--probe needs --probe-nro FILE, the edenprobe.nro to run "
                  "(github.com/scarletnx/edenprobe/releases): this build carries none");
        goto done;
    }
    if (!snx_is_file(nro)) {
        snx_error("--probe-nro %s: no such file", nro);
        goto done;
    }
    snx_tool_console();
    self = snx_executable(argv[0]);
    dir = self ? snx_dirname(self) : NULL;
    data = dir ? snx_data_dir(dir) : NULL;
    state = data ? snx_menu_dir(data, SNX_USER_STATE) : NULL;
    folder = state ? snx_path_join(state, "probe") : NULL;
    if (!folder) {
        snx_error("no state folder to run edenprobe in: %s", !self   ? "this program's path is not known"
                                                             : !data ? "no data folder (no home)"
                                                                     : "the data folder names no state folder");
        rc = 1;
        goto done;
    }
    probe.self = self;
    probe.nro = nro;
    probe.mode = mode;
    probe.folder = folder;
    probe.settings = settings;
    probe.quiet = SNX_PROBE_QUIET;
    probe.launches = SNX_PROBE_LAUNCHES;
    probe.out = stdout;
    rc = snx_probe_run(&probe);
done:
    free((void*)settings);
    free(self);
    free(dir);
    free(data);
    free(state);
    free(folder);
    return rc;
#endif
}
