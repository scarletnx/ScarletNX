/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The MCP server's edenprobe tools (mcp_int.h): from a confirmed game report to an edenprobe case that asks the
   hardware the question the failure narrowed down to, as RFC #347 has it. edenprobe (scarletnx/edenprobe) is a
   homebrew program of such cases; ScarletNX's CI holds every build to the cases that passed before.

   A service case is a row and an enum entry (source/serviceref.[ch]), a judge (there too) that holds what the case saw
   to what the console answers, the judge's reference check with answers made by hand (source/tests/t_serviceref.c),
   the device side that asks the service (source/tests/t_service.c), and its suite's row in source/ui.c. The scaffold
   writes the parts that follow from the case's suite, name and tier, which edenprobe's layout fixes; what the case
   asks and expects is the agent's to write, from what the console or the reference (reden, Eden) answers. The tools
   that run something run it through snx_run: no shell, the output kept off the protocol's stdout */
#include "nx/runtime/driver/mcp_int.h"
#include "common/utilities/common.h"
#include <cJSON.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SCAFFOLD_FILES 5
#define MAX_LAUNCHES   6
#define RUN_SECONDS    "--seconds=900" /* a launch's limit: edenprobe ends itself after its summary */

/* ============================================================ text */
static char* slurp(const char* path) {
    size_t size = 0;
    unsigned char* data = snx_read_file(path, &size);
    char* text;
    if (!data) return NULL;
    text = (char*)snx_alloc(size + 1);
    memcpy(text, data, size);
    text[size] = 0;
    free(data);
    return text;
}
/* insert put at the offset at of text, which is freed: the new text */
static char* insert_at(char* text, size_t at, const char* insert) {
    size_t n = strlen(text), k = strlen(insert);
    char* out = (char*)snx_alloc(n + k + 1);
    memcpy(out, text, at);
    memcpy(out + at, insert, k);
    memcpy(out + at + k, text + at, n - at + 1);
    free(text);
    return out;
}
static const char* last_of(const char* text, const char* lit) {
    const char *at = strstr(text, lit), *last = NULL;
    while (at) {
        last = at;
        at = strstr(at + 1, lit);
    }
    return last;
}
static int all_of(const char* s, const char* allowed, size_t most) {
    size_t n = strlen(s);
    return n && n <= most && strspn(s, allowed) == n;
}
/* text in a markdown table cell: a code span, its backticks and bars made harmless */
static void cell(Text* t, const char* text) {
    mcp_put(t, "`");
    for (; *text; text++) mcp_put_n(t, *text == '`' ? "'" : *text == '|' ? "/" : text, 1);
    mcp_put(t, "`");
}

/* ============================================================ edenprobe_scaffold */
/* A clone on Windows has CRLF lines (git's autocrlf): its text is read with LF ones, which the anchors are written
   with, and written back with the ending it had. 1 when it had CRLF */
static int lf_lines(char* text) {
    char* out = text;
    const char* in;
    int crlf = strstr(text, "\r\n") != NULL;
    if (!crlf) return 0;
    for (in = text; *in; in++)
        if (!(in[0] == '\r' && in[1] == '\n')) *out++ = *in;
    *out = 0;
    return 1;
}
/* text with each LF as CRLF, malloc'd */
static char* crlf_lines(const char* text) {
    size_t n = 0;
    const char* in;
    char *out, *at;
    for (in = text; *in; in++) n += *in == '\n' ? 2 : 1;
    out = at = (char*)snx_alloc(n + 1);
    for (in = text; *in; in++) {
        if (*in == '\n') *at++ = '\r';
        *at++ = *in;
    }
    *at = 0;
    return out;
}

static const char* tier_constant(const char* tier) {
    static const char* const names[] = {"none", "exact", "ulp", "position", "level"};
    static const char* const constants[] = {"CHECK_NONE", "CHECK_EXACT", "CHECK_ULP", "CHECK_POSITION", "CHECK_LEVEL"};
    int i;
    for (i = 0; i < 5; i++)
        if (!strcmp(tier, names[i])) return constants[i];
    return NULL;
}

char* mcp_edenprobe_scaffold(const char* checkout, const char* suite, const char* id, const char* name,
                             const char* tier, long issue, Text* err) {
    static const char* const files[SCAFFOLD_FILES] = {"source/serviceref.h", "source/serviceref.c",
                                                      "source/tests/t_service.c", "source/tests/t_serviceref.c",
                                                      "source/ui.c"};
    char *path[SCAFFOLD_FILES], *text[SCAFFOLD_FILES], *snippet, camel[64], full[64], quoted[256];
    int crlf[SCAFFOLD_FILES];
    const char *constant = tier_constant(tier ? tier : ""), *at;
    Text out = {0};
    int i, k, new_suite;
    size_t c = 0;
    if (!all_of(suite, "abcdefghijklmnopqrstuvwxyz0123456789-", 32)) {
        mcp_put(err, "suite: the service's name in lower case, letters, digits and -, as edenprobe's suites are");
        return NULL;
    }
    if (!all_of(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", 40) || !isupper((unsigned char)id[0])) {
        mcp_put(err, "id: the case's enum name without SERVICE_, upper case and _ (HID_HOLD_TYPE)");
        return NULL;
    }
    for (i = 0; name[i]; i++)
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126 || name[i] == '"' || name[i] == '\\') break;
    if (name[i] || i < 8 || i > 160) {
        mcp_put(err, "name: one sentence of plain ASCII about the hardware's behaviour, 8 to 160 characters, without "
                     "quotes or backslashes");
        return NULL;
    }
    if (!constant) {
        mcp_put(err, "tier: none, exact, ulp, position or level (edenprobe's docs/Design.md)");
        return NULL;
    }
    if (issue <= 0) {
        mcp_put(err, "issue: the ScarletNX issue the case comes from");
        return NULL;
    }
    /* HID_HOLD_TYPE: HidHoldType, the judge's and the functions' name */
    for (i = 0, k = 0; id[i] && c < sizeof camel - 1; i++)
        if (id[i] == '_')
            k = 0;
        else { camel[c++] = (char)(k++ ? tolower((unsigned char)id[i]) : id[i]); }
    camel[c] = 0;
    sprintf(full, "SERVICE_%s", id);
    sprintf(quoted, "\"%s\"", name);
    for (i = 0; i < SCAFFOLD_FILES; i++) {
        path[i] = snx_path_join(checkout, files[i]);
        text[i] = slurp(path[i]);
        crlf[i] = text[i] ? lf_lines(text[i]) : 0;
    }
    for (i = 0; i < SCAFFOLD_FILES && text[i]; i++) {}
    if (i < SCAFFOLD_FILES) {
        mcp_put(err, "not an edenprobe checkout: no ");
        mcp_put(err, files[i]);
        goto done;
    }
    snippet = snx_format("%s,", full);
    k = strstr(text[0], snippet) != NULL || strstr(text[1], quoted) != NULL;
    free(snippet);
    snippet = snx_format("Run%s(", camel);
    k |= strstr(text[2], snippet) != NULL;
    free(snippet);
    if (k) {
        mcp_put(err, "edenprobe has that case already: its id, its name or its functions");
        goto done;
    }
    snippet = snx_format("{ \"%s\", UI_FAMILY_", suite);
    new_suite = !strstr(text[4], snippet);
    free(snippet);

    /* serviceref.h: the enum entry before SERVICE_CASES, the judge before the closing extern "C" */
    at = strstr(text[0], "    SERVICE_CASES\n} ServiceCaseId;");
    if (!at || !last_of(text[0], "\n#ifdef __cplusplus\n}")) goto layout;
    snippet = snx_format("    %s, ///< %s: %s\n", full, suite, name);
    text[0] = insert_at(text[0], (size_t)(at - text[0]), snippet);
    free(snippet);
    snippet = snx_format("\n/// %s (ScarletNX#%ld).\n"
                         "/// TODO: what the case saw, what the console answers and where that comes from (reden's\n"
                         "/// commit and file:line, or a console), and when it passes.\n"
                         "bool ServiceJudge%s(uint32_t rc, char *out, size_t out_size);\n",
                         name, issue, camel);
    text[0] = insert_at(text[0], (size_t)(last_of(text[0], "\n#ifdef __cplusplus\n}") - text[0]), snippet);
    free(snippet);

    /* serviceref.c: the row at the end of CASES[], the judge at the end of the file */
    at = strstr(text[1], "CASES[");
    at = at ? strstr(at, "\n};") : NULL;
    if (!at) goto layout;
    snippet = snx_format("    [%s] = { \"%s\",\n%*s\"%s\" },\n", full, suite, (int)(strlen(full) + 11), "", name);
    text[1] = insert_at(text[1], (size_t)(at - text[1]) + 1, snippet);
    free(snippet);
    snippet = snx_format("\nbool ServiceJudge%s(uint32_t rc, char *out, size_t out_size) {\n"
                         "    // TODO(ScarletNX#%ld): hold what the case saw to the console's answer.\n"
                         "    const bool passed = rc == 0u;\n"
                         "    snprintf(out, out_size, \"TODO: rc 0x%%x\", (unsigned)rc);\n"
                         "    return passed;\n}\n",
                         camel, issue);
    text[1] = insert_at(text[1], strlen(text[1]), snippet);
    free(snippet);

    /* t_service.c: the device side before RunService, and its call at RunService's end */
    at = strstr(text[2], "\nvoid RunService(void) {");
    if (!at || !strstr(at, "\n}")) goto layout;
    snippet = snx_format(
        "\n// ---------------------------------------------------------------------------------------------\n"
        "// %s: %s (ScarletNX#%ld)\n\n"
        "/// TODO: what the case asks of the service, the way a game asks it.\n"
        "static void Run%s(void) {\n"
        "    char detail[480];\n"
        "    if (!Start(%s)) {\n"
        "        return;\n"
        "    }\n"
        "    // TODO: ask the service, and keep what it answers for the judge.\n"
        "    const uint32_t rc = 0u;\n"
        "    const bool passed = ServiceJudge%s(rc, detail, sizeof(detail));\n"
        "    Report(%s, %s, passed, detail);\n"
        "}\n",
        suite, name, issue, camel, full, camel, full, constant);
    text[2] = insert_at(text[2], (size_t)(at - text[2]), snippet);
    free(snippet);
    at = strstr(strstr(text[2], "\nvoid RunService(void) {"), "\n}");
    snippet = snx_format("\n    Run%s();", camel);
    text[2] = insert_at(text[2], (size_t)(at - text[2]), snippet);
    free(snippet);

    /* t_serviceref.c: the judge's check before RunReferenceService, and its call at that one's end */
    at = strstr(text[3], "\nvoid RunReferenceService(void) {");
    if (!at || !strstr(at, "\n}")) goto layout;
    snippet =
        snx_format("\nstatic void Check%s(void) {\n"
                   "    // TODO(ScarletNX#%ld): the console's answer passes, and each wrong one fails, ScarletNX's\n"
                   "    // among them.\n"
                   "    const bool right = ServiceJudge%s(0u, said, sizeof(said));\n"
                   "    const bool wrong = !ServiceJudge%s(SERVICE_RESULT(1, 1), said, sizeof(said));\n"
                   "    CHECK_TRUE(SUITE, \"service: %s passes TODO\", CHECK_NONE, right && wrong,\n"
                   "               \"right %%d, wrong %%d\", right, wrong);\n"
                   "}\n",
                   camel, issue, camel, camel, suite);
    text[3] = insert_at(text[3], (size_t)(at - text[3]), snippet);
    free(snippet);
    at = strstr(strstr(text[3], "\nvoid RunReferenceService(void) {"), "\n}");
    snippet = snx_format("\n    Check%s();", camel);
    text[3] = insert_at(text[3], (size_t)(at - text[3]), snippet);
    free(snippet);

    /* ui.c: a new suite's row after the service family's last */
    if (new_suite) {
        at = last_of(text[4], "UI_FAMILY_SERVICE },");
        at = at ? strchr(at, '\n') : NULL;
        if (!at) goto layout;
        snippet = snx_format("    { \"%s\", UI_FAMILY_SERVICE },\n", suite);
        text[4] = insert_at(text[4], (size_t)(at - text[4]) + 1, snippet);
        free(snippet);
    }

    for (i = 0; i < SCAFFOLD_FILES; i++)
        if (crlf[i]) {
            char* ended = crlf_lines(text[i]);
            free(text[i]);
            text[i] = ended;
        }
    for (i = 0; i < SCAFFOLD_FILES; i++)
        if ((i < 4 || new_suite) && snx_write_file(path[i], text[i], strlen(text[i]))) {
            mcp_put(err, "could not write ");
            mcp_put(err, path[i]);
            goto done;
        }
    mcp_put(&out, "Written into ");
    mcp_put(&out, checkout);
    mcp_put(&out, ":\n- source/serviceref.h: ");
    mcp_put(&out, full);
    mcp_put(&out, " and ServiceJudge");
    mcp_put(&out, camel);
    mcp_put(&out,
            "'s declaration\n- source/serviceref.c: its CASES[] row and the judge\n- source/tests/t_service.c: Run");
    mcp_put(&out, camel);
    mcp_put(&out, ", called at RunService's end\n- source/tests/t_serviceref.c: Check");
    mcp_put(&out, camel);
    mcp_put(&out, ", called at RunReferenceService's end\n");
    if (new_suite) mcp_put(&out, "- source/ui.c: the new suite's row in the service family\n");
    mcp_put(&out, "\nLeft to write (every TODO names the issue):\n"
                  "1. The device side: the service call in Run");
    mcp_put(&out, camel);
    mcp_put(&out, ", asked the way a game asks it (libnx, or the command id and buffers), and what it keeps.\n");
    mcp_put(&out, "2. The judge's arguments and logic: what the case saw against what the console answers. Cite "
                  "where the answer comes from in its comment: reden's commit and file:line, or a console's run.\n");
    mcp_put(&out, "3. The reference check: the right answer passes, each wrong one fails (ScarletNX's among them), "
                  "and its name says what passes. The \"said\" detail can be held to a prefix, as the others do.\n");
    mcp_put(&out, "4. The docs, in the same commit: the suite's row and count in docs/families/Service.md, the "
                  "counts in words in docs/families/README.md, docs/families/Judge.md and docs/Console.md (one "
                  "more reference case), and an open question in Service.md while no console has answered.\n");
    mcp_put(&out, "5. Nothing of the game: no code, disassembly, assets or values only that game uses. The case "
                  "asks the hardware.\n");
    mcp_put(&out, "\nThen edenprobe_check runs the judges on the host, the NRO built from the checkout "
                  "(devkitPro's image, or the pull request's CI artifact) goes to edenprobe_run, and "
                  "edenprobe_finding writes the pull request's text.\n");
    goto done;
layout:
    mcp_put(err, "edenprobe's layout is not the one the scaffold knows (an anchor is missing): nothing was written, "
                 "write the case by hand");
done:
    for (i = 0; i < SCAFFOLD_FILES; i++) {
        free(path[i]);
        free(text[i]);
    }
    if (err->n) {
        free(out.p);
        return NULL;
    }
    return out.p;
}

/* ============================================================ edenprobe_check */
/* A line of a run's output worth keeping: about suite, a failure or the summary; not a compile command */
static int worth(const char* line, const char* suite) {
    if (!strncmp(line, "cc ", 3) || !strncmp(line, "gcc ", 4) || !strncmp(line, "c++ ", 4) ||
        !strncmp(line, "clang", 5))
        return strstr(line, "error:") != NULL;
    return strstr(line, suite) || strstr(line, "\"result\":\"fail\"") || strstr(line, "FAIL") ||
           strstr(line, "error:") || strstr(line, "Error") || strstr(line, "passed=") ||
           strstr(line, "\"type\":\"summary\"");
}
/* the lines of output worth keeping, and the last ones, MAX_KEPT at most */
#define MAX_KEPT 200
static void keep_lines(Text* t, const char* output, const char* suite) {
    const char *line = output, *end;
    size_t total = 0, n, from;
    for (end = output; *end; end++) total += *end == '\n';
    from = total > 10 ? total - 10 : 0;
    for (n = 0; *line && t->lines < MAX_KEPT; n++) {
        Text l = {0};
        end = strchr(line, '\n');
        mcp_put_n(&l, line, end ? (size_t)(end - line) : strlen(line));
        /* a line kept already (a report printed twice) goes */
        if (l.p && (n >= from || worth(l.p, suite)) && !(t->p && strstr(t->p, l.p))) {
            mcp_put(t, l.p);
            mcp_put(t, "\n");
            t->lines++;
        }
        free(l.p);
        if (!end) break;
        line = end + 1;
    }
}

char* mcp_edenprobe_check(const char* checkout, const char* suite, Text* err) {
    static const char* const argv[] = {"make", "-f", "host.mk", "host-run", NULL};
    char *host = snx_path_join(checkout, "host.mk"), *output = NULL;
    Text out = {0};
    int rc;
    if (!snx_is_file(host)) {
        free(host);
        mcp_put(err, "not an edenprobe checkout: no host.mk");
        return NULL;
    }
    free(host);
    rc = snx_run(argv, checkout, NULL, &output);
    mcp_put(&out, rc ? "make -f host.mk host-run failed (exit " : "make -f host.mk host-run passed (exit ");
    mcp_put_num(&out, (unsigned long)(rc < 0 ? 255 : rc));
    mcp_put(&out, "). Its lines about the suite, its failures, its summary and its end. A test_judge_counts failure "
                  "(\"349 != 350\") is the docs' counts in words, left to update for a new case:\n");
    keep_lines(&out, output ? output : "", suite && suite[0] ? suite : "service");
    free(output);
    return out.p;
}

/* ============================================================ edenprobe_run */
/* FNV-1a of the NRO: whether the recompiled one is still its */
static unsigned long fingerprint(const unsigned char* data, size_t size) {
    unsigned long h = 2166136261UL;
    size_t i;
    for (i = 0; i < size; i++) h = ((h ^ data[i]) * 16777619UL) & 0xffffffffUL;
    return h;
}

/* the report the latest launch wrote (sdmc:/edenprobe/last-run names its folder), or NULL; its path through *where */
static char* report_text(const char* sdmc, char** where) {
    char *last = snx_path_join(sdmc, "last-run"), *name = slurp(last), *folder, *path, *text;
    size_t n;
    free(last);
    if (!name) return NULL;
    for (n = strlen(name); n && isspace((unsigned char)name[n - 1]); n--) name[n - 1] = 0;
    folder = snx_path_join(sdmc, name);
    path = snx_path_join(folder, "report.jsonl");
    text = slurp(path);
    free(name);
    free(folder);
    free(*where);
    *where = path;
    return text;
}
static int count_of(const char* report, const char* lit) {
    int n = 0;
    const char* at = report ? strstr(report, lit) : NULL;
    for (; at; at = strstr(at + 1, lit)) n++;
    return n;
}

/* a report's lines: its first note (the build), the cases filter names ("suite" or "suite: name"), its summary */
static void report_lines(Text* t, const char* report, const char* filter) {
    const char* line = report;
    char suite[64] = "";
    const char* name = strstr(filter, ": ");
    int notes = 0, cases = 0;
    size_t n = name ? (size_t)(name - filter) : strlen(filter);
    if (n >= sizeof suite) n = sizeof suite - 1;
    memcpy(suite, filter, n);
    suite[n] = 0;
    if (name) name += 2;
    while (line && *line) {
        const char* end = strchr(line, '\n');
        Text l = {0};
        cJSON* j;
        mcp_put_n(&l, line, end ? (size_t)(end - line) : strlen(line));
        j = l.p ? cJSON_Parse(l.p) : NULL;
        if (j) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "type"),
                        *s = cJSON_GetObjectItemCaseSensitive(j, "suite"),
                        *c = cJSON_GetObjectItemCaseSensitive(j, "name");
            int keep = 0;
            if (cJSON_IsString(type) && !strcmp(type->valuestring, "note"))
                keep = !notes++;
            else if (cJSON_IsString(type) && !strcmp(type->valuestring, "summary"))
                keep = 1;
            else if (cJSON_IsString(type) && !strcmp(type->valuestring, "case") && cJSON_IsString(s) &&
                     !strcmp(s->valuestring, suite) && (!name || (cJSON_IsString(c) && strstr(c->valuestring, name))))
                keep = ++cases <= MAX_KEPT;
            if (keep) {
                mcp_put(t, l.p);
                mcp_put(t, "\n");
            }
            cJSON_Delete(j);
        }
        free(l.p);
        line = end ? end + 1 : NULL;
    }
    if (!cases) {
        mcp_put(t, "(no case of ");
        mcp_put(t, filter);
        mcp_put(t, " in the report: is its family among the mode's words, and the NRO built with it?)\n");
    }
}

char* mcp_edenprobe_run(const char* self, const char* cache, const char* nro, const char* mode, const char* filter,
                        Text* err) {
#ifdef _WIN32
    static const char exe[] = "edenprobe.exe";
#else
    static const char exe[] = "edenprobe";
#endif
    char *work = snx_path_join(cache, "edenprobe-run"), *games = snx_path_join(work, "games"),
         *folder = snx_path_join(games, "edenprobe"), *game = snx_path_join(folder, exe),
         *stamp_path = snx_path_join(work, "nro.stamp"), *data = snx_path_join(folder, "edenprobe_data"),
         *sdmc = snx_path_join(data, "sdmc/edenprobe"), *mode_path = snx_path_join(sdmc, "mode"),
         *last = snx_path_join(sdmc, "last-run"), *old_stamp = slurp(stamp_path), *stamp = NULL, *output = NULL,
         *report = NULL, *report_path = NULL;
    unsigned char* bytes;
    size_t size = 0;
    Text out = {0};
    int launch, cases = -1;
    bytes = snx_read_file(nro, &size);
    if (!bytes) {
        mcp_put(err, "the NRO cannot be read");
        goto done;
    }
    stamp = snx_format("%lu %lx\n", (unsigned long)size, fingerprint(bytes, size));
    free(bytes);
    if (!old_stamp || strcmp(old_stamp, stamp) || !snx_is_file(game)) {
        const char* argv[8];
        int rc;
        argv[0] = self;
        argv[1] = "-r";
        argv[2] = nro;
        argv[3] = "edenprobe";
        argv[4] = "--outdir";
        argv[5] = games;
        argv[6] = NULL;
        if (snx_path_exists(games)) snx_remove_tree(games);
        snx_mkdirs(games);
        /* HOME in the work folder: nothing of the person's ScarletNX folders is read or written */
        rc = snx_run(argv, work, work, &output);
        if (rc || !snx_is_file(game)) {
            mcp_put(err, "the recompile failed: ");
            keep_lines(err, output ? output : "", "error");
            goto done;
        }
        snx_write_file(stamp_path, stamp, strlen(stamp));
        free(output);
        output = NULL;
    }
    {
        Text words = {0};
        int failed;
        mcp_put(&words, mode);
        mcp_put(&words, "\n");
        failed = snx_mkdirs(sdmc) || snx_write_file(mode_path, words.p, words.n);
        free(words.p);
        if (failed) {
            mcp_put(err, "the mode file cannot be written");
            goto done;
        }
    }
    if (snx_path_exists(last)) remove(last); /* a launch that writes no report leaves none, not the last one's */
    for (launch = 1; launch <= MAX_LAUNCHES; launch++) {
        const char* argv[4];
        int now;
        argv[0] = game;
        argv[1] = "-H";
        argv[2] = RUN_SECONDS;
        argv[3] = NULL;
        snx_run(argv, folder, work, &output);
        free(output);
        output = NULL;
        free(report);
        report = report_text(sdmc, &report_path);
        if (!report) {
            mcp_put(err, "the launch left no report");
            goto done;
        }
        if (strstr(report, "\"type\":\"summary\"") || strstr(report, "\"type\": \"summary\"")) break;
        now = count_of(report, "\"type\":\"case\"") + count_of(report, "\"type\": \"case\"");
        if (now <= cases) break; /* it would only end the same way again */
        cases = now;
    }
    mcp_put(&out, "edenprobe in mode \"");
    mcp_put(&out, mode);
    mcp_put(&out, "\", ");
    mcp_put_num(&out, (unsigned long)(launch > MAX_LAUNCHES ? MAX_LAUNCHES : launch));
    mcp_put(&out, " launch(es). The report's build, the cases of ");
    mcp_put(&out, filter);
    mcp_put(&out, " and its summary:\n");
    report_lines(&out, report, filter);
    if (!strstr(report, "summary")) mcp_put(&out, "(no summary: the run ended before it)\n");
    mcp_put(&out, "The report, for edenprobe_finding: ");
    mcp_put(&out, report_path);
    mcp_put(&out, "\n");
done:
    free(work);
    free(games);
    free(folder);
    free(game);
    free(stamp_path);
    free(data);
    free(sdmc);
    free(mode_path);
    free(last);
    free(old_stamp);
    free(stamp);
    free(output);
    free(report);
    free(report_path);
    if (err->n) {
        free(out.p);
        return NULL;
    }
    return out.p;
}

/* ============================================================ edenprobe_finding */
typedef struct {
    char *build, *result, *detail;
} Line;

/* the case's line in a report.jsonl, and the build its first note names; 0, or -1 when the file can't be read */
static int find_case(const char* path, const char* suite, const char* name, Line* found) {
    char* report = slurp(path);
    const char* line = report;
    if (!report) return -1;
    while (line && *line) {
        const char* end = strchr(line, '\n');
        Text l = {0};
        cJSON* j;
        mcp_put_n(&l, line, end ? (size_t)(end - line) : strlen(line));
        j = l.p ? cJSON_Parse(l.p) : NULL;
        if (j) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "type"),
                        *text = cJSON_GetObjectItemCaseSensitive(j, "text"),
                        *s = cJSON_GetObjectItemCaseSensitive(j, "suite"),
                        *c = cJSON_GetObjectItemCaseSensitive(j, "name"),
                        *r = cJSON_GetObjectItemCaseSensitive(j, "result"),
                        *d = cJSON_GetObjectItemCaseSensitive(j, "detail");
            if (!found->build && cJSON_IsString(type) && !strcmp(type->valuestring, "note") && cJSON_IsString(text))
                found->build = snx_strdup(text->valuestring);
            if (!found->result && cJSON_IsString(s) && !strcmp(s->valuestring, suite) && cJSON_IsString(c) &&
                !strcmp(c->valuestring, name) && cJSON_IsString(r)) {
                found->result = snx_strdup(r->valuestring);
                found->detail = snx_strdup(cJSON_IsString(d) ? d->valuestring : "");
            }
            cJSON_Delete(j);
        }
        free(l.p);
        line = end ? end + 1 : NULL;
    }
    free(report);
    return 0;
}
static void line_free(Line* l) {
    free(l->build);
    free(l->result);
    free(l->detail);
}
static void row(Text* t, const char* machine, const Line* l) {
    mcp_put(t, "| ");
    mcp_put(t, machine);
    if (l->build) {
        mcp_put(t, " (");
        cell(t, l->build);
        mcp_put(t, ")");
    }
    mcp_put(t, " | ");
    mcp_put(t, l->result);
    mcp_put(t, " | ");
    cell(t, l->detail);
    mcp_put(t, " |\n");
}

char* mcp_edenprobe_finding(long issue, const char* suite, const char* name, const char* ours, const char* theirs,
                            const char* source, Text* err) {
    Line us = {0}, them = {0};
    Text out = {0};
    if (issue <= 0 || !suite || !name || !ours) {
        mcp_put(err, "issue, suite, name and scarletnx_report (the report.jsonl of edenprobe_run's run) are needed");
        return NULL;
    }
    if (find_case(ours, suite, name, &us) || (theirs && find_case(theirs, suite, name, &them))) {
        mcp_put(err, "a report cannot be read");
        goto done;
    }
    if (!us.result) {
        mcp_put(err, "the case is not in ScarletNX's report: run it (edenprobe_run) with its family in the mode");
        goto done;
    }
    {
        char* subject = snx_format("edenprobe: %s, %s", suite, name);
        size_t n = strlen(subject);
        mcp_put(&out, "Commit subject: ");
        mcp_put(&out, subject);
        mcp_put(&out, n > 72 ? "\n(edenprobe's subjects are 72 characters at most, this one is " : "\n(");
        if (n > 72) {
            mcp_put_num(&out, (unsigned long)n);
            mcp_put(&out, ": shorten it. ");
        }
        mcp_put(&out, "ASCII; hooks/commit-msg adds the Change-Id, and a Signed-off-by is needed)\n\n");
        free(subject);
    }
    if (!strcmp(us.result, "pass"))
        mcp_put(&out, "WARNING: the case passes on ScarletNX, so it does not show the issue yet.\n\n");
    if (them.result && strcmp(them.result, "pass"))
        mcp_put(&out, "WARNING: the case does not pass on the reference: it may hold a wrong answer.\n\n");
    if (theirs && !them.result) mcp_put(&out, "WARNING: the case is not in the reference's report.\n\n");
    mcp_put(&out, "---\n\n### ");
    mcp_put(&out, suite);
    mcp_put(&out, ": ");
    mcp_put(&out, name);
    mcp_put(&out, "\n\nAsked because of scarletnx/ScarletNX#");
    mcp_put_num(&out, (unsigned long)issue);
    mcp_put(&out, ", where a game failed on ScarletNX's answer to it. ");
    mcp_put(
        &out,
        source && source[0]
            ? source
            : "TODO: what the console answers, and where that comes from (reden's commit and file:line, or a console's run).");
    mcp_put(&out, "\n\n| machine | result | detail |\n| --- | --- | --- |\n");
    row(&out, "ScarletNX", &us);
    if (them.result) row(&out, "reference", &them);
    mcp_put(&out,
            "\nTesting: TODO from edenprobe_check (`make -f host.mk host-run`: its summary, and the judge's wrong "
            "answers, ScarletNX's among them, failing in its reference check) and edenprobe_run (the mode it ran "
            "the NRO in on ScarletNX");
    mcp_put(&out, them.result ? ", and the reference's run above).\n" : "; TODO: the reference's run).\n");
    mcp_put(&out, "\nBefore opening it:\n"
                  "- nothing of a game in the case: no code, no disassembly, no assets, no value only that game uses\n"
                  "- the expected answer cited in the judge's comment\n"
                  "- the docs in the same commit: docs/families/Service.md (the suite's row and count, an open "
                  "question while no console has run it), the counts in docs/families/README.md, Judge.md and "
                  "docs/Console.md\n");
done:
    line_free(&us);
    line_free(&them);
    if (err->n) {
        free(out.p);
        return NULL;
    }
    return out.p;
}
