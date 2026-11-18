/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vkcmdgen: the device commands each Vulkan device extension requires, from the Khronos registry (vk.xml of the
   Vulkan-Headers the renderer builds with), as the table common/gpu/vk/extension_commands.h declares: every extension
   whose type is device and which Vulkan supports, sorted by name, each with the commands its <require> blocks name
   that a device answers for (the first parameter a dispatchable handle of a device: VkDevice, VkQueue, VkCommandBuffer
   and the like; vkGetDeviceProcAddr gives no others), and the block's depends where it has one (the commands are a
   device's only when that holds). Blocks for another API (Vulkan SC) are left out. A command an extension names that
   the registry defines nowhere, an extension named twice, or a character a C string would need escaped stops it,
   named with its line. It is strict ISO C23 and depends on nothing, so a cross build can build it for the machine it
   runs on.

   Usage: vkcmdgen REGISTRY OUT.c (rewritten only when it changes) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HANDLES    64
#define MAX_COMMANDS   8192 /* definitions and aliases */
#define MAX_EXTENSIONS 2048
#define MAX_REQUIRED   8192 /* (extension, command) pairs */

typedef struct {
    size_t name, name_n;     /* in the text */
    size_t target, target_n; /* an alias's command, else 0 */
    int device;              /* a definition's: its first parameter a device's dispatchable handle */
} Command;
typedef struct {
    size_t name, name_n;
    size_t tag;            /* its tag's offset, for errors */
    unsigned first, count; /* its commands in g_vkcmdgen.required */
} Extension;
typedef struct {
    size_t name, name_n;       /* as the extension's <command> names it */
    size_t depends, depends_n; /* its <require>'s depends, depends_n 0 for none */
} Required;

static struct {
    const char* path;
    char* text;
    size_t handle[MAX_HANDLES], handle_n[MAX_HANDLES];
    unsigned handles;
    Command commands[MAX_COMMANDS];
    unsigned ncommands;
    Extension extensions[MAX_EXTENSIONS];
    unsigned nextensions;
    Required required[MAX_REQUIRED];
    unsigned nrequired;
    char* out;
    size_t out_n, out_cap;
} g_vkcmdgen;

/* the line of the text's byte at */
static size_t line_of(size_t at) {
    size_t line = 1;
    for (size_t i = 0; i < at && g_vkcmdgen.text[i]; i++) line += g_vkcmdgen.text[i] == '\n';
    return line;
}

static void fail(size_t at, const char* fmt, ...) {
    va_list ap;
    if (at)
        fprintf(stderr, "%s:%zu: ", g_vkcmdgen.path, line_of(at));
    else
        fprintf(stderr, "vkcmdgen: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    free(g_vkcmdgen.text);
    free(g_vkcmdgen.out);
    exit(1);
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    long n = 0;
    char* text;
    if (!f) fail(0, "cannot read %s", path);
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        fail(0, "cannot read %s", path);
    }
    text = (char*)malloc((size_t)n + 1);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        fclose(f);
        fail(0, "cannot read %s", path);
    }
    text[n] = 0;
    fclose(f);
    return text;
}

/* the text from at as a pointer */
static const char* at_(size_t at) { return g_vkcmdgen.text + at; }

/* the next "<tag" at or after from and before end (a tag of that name, not a longer one): its offset, 0 for none */
static size_t find_tag(size_t from, size_t end, const char* tag) {
    size_t n = strlen(tag);
    const char* p = at_(from);
    while ((p = strstr(p, tag)) && (size_t)(p - g_vkcmdgen.text) < end) {
        char c = p[n];
        if (c == ' ' || c == '>' || c == '/' || c == '\n' || c == '\t') return (size_t)(p - g_vkcmdgen.text);
        p += n;
    }
    return 0;
}
/* the offset of the first s at or after from, before end; 0 for none */
static size_t find(size_t from, size_t end, const char* s) {
    const char* p = strstr(at_(from), s);
    return p && (size_t)(p - g_vkcmdgen.text) < end ? (size_t)(p - g_vkcmdgen.text) : 0;
}
/* the end of the tag that starts at tag (its '>'), and whether it closes itself (/>) */
static size_t tag_end(size_t tag, int* closed) {
    const char* p = strchr(at_(tag), '>');
    if (!p) fail(tag, "a tag with no end");
    *closed = p > at_(tag) && p[-1] == '/';
    return (size_t)(p - g_vkcmdgen.text);
}
/* the value of the tag's attribute name: its offset and length in *n, 0 for none */
static size_t attribute(size_t tag, const char* name, size_t* n) {
    size_t len = strlen(name);
    int closed;
    size_t end = tag_end(tag, &closed);
    for (size_t p = tag + 1; p + len + 2 < end; p++) {
        const char* s = at_(p);
        if ((s[-1] == ' ' || s[-1] == '\n' || s[-1] == '\t') && !strncmp(s, name, len) && s[len] == '=' &&
            s[len + 1] == '"') {
            const char* q = strchr(s + len + 2, '"');
            if (!q || (size_t)(q - g_vkcmdgen.text) > end) fail(tag, "attribute %s has no end", name);
            *n = (size_t)(q - (s + len + 2));
            return p + len + 2;
        }
    }
    *n = 0;
    return 0;
}
/* whether the n bytes at at are s */
static int text_is(size_t at, size_t n, const char* s) { return strlen(s) == n && !strncmp(at_(at), s, n); }
/* whether word is one of the comma-separated words of the n bytes at at */
static int has_word(size_t at, size_t n, const char* word) {
    size_t len = strlen(word), i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && at_(at)[j] != ',') j++;
        if (j - i == len && !strncmp(at_(at) + i, word, len)) return 1;
        i = j + 1;
    }
    return 0;
}
/* the n bytes at at, each a letter, a digit or one of extra: a C string holds them as they are */
static void plain(size_t at, size_t n, const char* extra, const char* what) {
    for (size_t i = 0; i < n; i++) {
        char c = at_(at)[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              (c && strchr(extra, c))))
            fail(at, "%s \"%.*s\" has '%c', which the table does not take", what, (int)n, at_(at), c);
    }
    if (!n) fail(at, "an empty %s", what);
}

/* the dispatchable handles (VK_DEFINE_HANDLE): what a device answers for is a command on one of them that is not the
   instance's or a physical device's */
static void read_handles(void) {
    size_t at = 0;
    while ((at = find(at + 1, (size_t)-1, "<type>VK_DEFINE_HANDLE</type>(<name>"))) {
        size_t name = at + strlen("<type>VK_DEFINE_HANDLE</type>(<name>"), end = find(name, (size_t)-1, "</name>");
        if (!end) fail(at, "a handle with no end to its name");
        if (g_vkcmdgen.handles == MAX_HANDLES) fail(at, "past the %d dispatchable handles this reads", MAX_HANDLES);
        g_vkcmdgen.handle[g_vkcmdgen.handles] = name;
        g_vkcmdgen.handle_n[g_vkcmdgen.handles++] = end - name;
    }
    if (!g_vkcmdgen.handles) fail(0, "%s defines no dispatchable handle (VK_DEFINE_HANDLE)", g_vkcmdgen.path);
}
static int device_handle(size_t at, size_t n) {
    if (text_is(at, n, "VkInstance") || text_is(at, n, "VkPhysicalDevice")) return 0;
    for (unsigned i = 0; i < g_vkcmdgen.handles; i++)
        if (g_vkcmdgen.handle_n[i] == n && !strncmp(at_(g_vkcmdgen.handle[i]), at_(at), n)) return 1;
    return 0;
}

/* <commands>: each definition for Vulkan, by its prototype's name and its first parameter's type, and each alias */
static void read_commands(void) {
    size_t start = find_tag(0, (size_t)-1, "<commands"), end, at;
    if (!start) fail(0, "%s has no <commands>", g_vkcmdgen.path);
    end = find(start, (size_t)-1, "</commands>");
    if (!end) fail(start, "<commands> has no end");
    for (at = start + 1; (at = find_tag(at, end, "<command")); at++) {
        int closed;
        size_t close = tag_end(at, &closed), n, api = attribute(at, "api", &n);
        Command c = {0};
        if (g_vkcmdgen.ncommands == MAX_COMMANDS) fail(at, "past the %d commands this reads", MAX_COMMANDS);
        if (api && !has_word(api, n, "vulkan")) { /* another API's (Vulkan SC's) definition */
            if (!closed && !(at = find(close, end, "</command>"))) fail(close, "a command with no end");
            continue;
        }
        c.target = attribute(at, "alias", &c.target_n);
        if (c.target) {
            c.name = attribute(at, "name", &c.name_n);
            if (!c.name) fail(at, "an alias with no name");
        } else {
            size_t stop = find(close, end, "</command>"), proto, param, type, type_end;
            if (!stop) fail(close, "a command with no end");
            proto = find(close, stop, "<proto>");
            c.name = proto ? find(proto, stop, "<name>") : 0;
            if (!c.name) fail(at, "a command with no prototype's name");
            c.name += strlen("<name>");
            n = find(c.name, stop, "</name>");
            if (!n) fail(c.name, "a command's name with no end");
            c.name_n = n - c.name;
            param = find_tag(c.name, stop, "<param");
            type = param ? find(param, stop, "<type>") : 0;
            if (type) {
                type += strlen("<type>");
                type_end = find(type, stop, "</type>");
                if (!type_end) fail(type, "a parameter's type with no end");
                c.device = device_handle(type, type_end - type);
            }
            at = stop;
        }
        g_vkcmdgen.commands[g_vkcmdgen.ncommands++] = c;
    }
}
/* the command a name defines, an alias followed to its definition: -1 for none */
static int command_of(size_t at, size_t n, unsigned depth) {
    for (unsigned i = 0; i < g_vkcmdgen.ncommands; i++) {
        const Command* c = &g_vkcmdgen.commands[i];
        if (c->name_n != n || strncmp(at_(c->name), at_(at), n)) continue;
        if (!c->target) return (int)i;
        if (depth > 8) fail(at, "%.*s is an alias of an alias past 8 deep", (int)n, at_(at));
        return command_of(c->target, c->target_n, depth + 1);
    }
    return -1;
}

static int by_name(const void* a, const void* b) {
    const Extension *x = (const Extension*)a, *y = (const Extension*)b;
    size_t n = x->name_n < y->name_n ? x->name_n : y->name_n;
    int c = strncmp(at_(x->name), at_(y->name), n);
    return c ? c : (x->name_n > y->name_n) - (x->name_n < y->name_n);
}

/* <extensions>: each device extension Vulkan supports, with the device commands of its blocks for Vulkan */
static void read_extensions(void) {
    size_t start = find_tag(0, (size_t)-1, "<extensions"), end, at;
    if (!start) fail(0, "%s has no <extensions>", g_vkcmdgen.path);
    end = find(start, (size_t)-1, "</extensions>");
    if (!end) fail(start, "<extensions> has no end");
    for (at = start + 1; (at = find_tag(at, end, "<extension")); at++) {
        int closed;
        size_t close = tag_end(at, &closed), name_n, type_n, supported_n;
        size_t name = attribute(at, "name", &name_n), type = attribute(at, "type", &type_n),
               supported = attribute(at, "supported", &supported_n);
        size_t stop = closed ? close : find(close, end, "</extension>");
        if (!name) fail(at, "an extension with no name");
        if (!stop) fail(at, "%.*s has no end", (int)name_n, at_(name));
        if (!type || !text_is(type, type_n, "device") || !supported || !has_word(supported, supported_n, "vulkan")) {
            at = stop;
            continue;
        }
        plain(name, name_n, "", "an extension's name");
        if (g_vkcmdgen.nextensions == MAX_EXTENSIONS) fail(at, "past the %d extensions this reads", MAX_EXTENSIONS);
        Extension* e = &g_vkcmdgen.extensions[g_vkcmdgen.nextensions++];
        *e = (Extension){name, name_n, at, g_vkcmdgen.nrequired, 0};
        for (size_t r = close; (r = find_tag(r + 1, stop, "<require"));) {
            int empty;
            size_t r_close = tag_end(r, &empty), api_n, depends_n, api = attribute(r, "api", &api_n),
                   depends = attribute(r, "depends", &depends_n);
            size_t r_stop = empty ? r_close : find(r_close, stop, "</require>");
            if (!r_stop) fail(r, "a <require> of %.*s has no end", (int)name_n, at_(name));
            if (api && !has_word(api, api_n, "vulkan")) {
                r = r_stop;
                continue;
            }
            if (depends) plain(depends, depends_n, ",+():", "a depends");
            for (size_t c = r_close; (c = find_tag(c + 1, r_stop, "<command"));) {
                size_t cmd_n, cmd = attribute(c, "name", &cmd_n);
                int def;
                if (!cmd) fail(c, "a command of %.*s with no name", (int)name_n, at_(name));
                plain(cmd, cmd_n, "", "a command's name");
                def = command_of(cmd, cmd_n, 0);
                if (def < 0)
                    fail(c, "%.*s requires %.*s, which the registry defines nowhere", (int)name_n, at_(name),
                         (int)cmd_n, at_(cmd));
                if (!g_vkcmdgen.commands[def].device) continue;
                if (g_vkcmdgen.nrequired == MAX_REQUIRED) fail(c, "past the %d commands this lists", MAX_REQUIRED);
                g_vkcmdgen.required[g_vkcmdgen.nrequired++] = (Required){cmd, cmd_n, depends, depends_n};
                e->count++;
            }
            r = r_stop;
        }
        at = stop;
    }
    if (!g_vkcmdgen.nextensions) fail(0, "%s has no device extension Vulkan supports", g_vkcmdgen.path);
    qsort(g_vkcmdgen.extensions, g_vkcmdgen.nextensions, sizeof *g_vkcmdgen.extensions, by_name);
    for (unsigned i = 1; i < g_vkcmdgen.nextensions; i++)
        if (!by_name(&g_vkcmdgen.extensions[i - 1], &g_vkcmdgen.extensions[i]))
            fail(g_vkcmdgen.extensions[i].tag, "%.*s is named twice", (int)g_vkcmdgen.extensions[i].name_n,
                 at_(g_vkcmdgen.extensions[i].name));
}

static void out(const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) fail(0, "cannot format %s", fmt);
    if (g_vkcmdgen.out_n + (size_t)n + 1 > g_vkcmdgen.out_cap) {
        size_t cap = (g_vkcmdgen.out_n + (size_t)n + 1) * 2;
        char* grown = (char*)realloc(g_vkcmdgen.out, cap);
        if (!grown) fail(0, "out of memory");
        g_vkcmdgen.out = grown;
        g_vkcmdgen.out_cap = cap;
    }
    va_start(ap, fmt);
    vsnprintf(g_vkcmdgen.out + g_vkcmdgen.out_n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    g_vkcmdgen.out_n += (size_t)n;
}

/* the header's version, as the registry defines VK_HEADER_VERSION */
static unsigned long header_version(void) {
    size_t at = find(0, (size_t)-1, "<name>VK_HEADER_VERSION</name>");
    char* end;
    unsigned long v;
    if (!at) fail(0, "%s defines no VK_HEADER_VERSION", g_vkcmdgen.path);
    v = strtoul(at_(at + strlen("<name>VK_HEADER_VERSION</name>")), &end, 10);
    if (end == at_(at + strlen("<name>VK_HEADER_VERSION</name>"))) fail(at, "VK_HEADER_VERSION has no number");
    return v;
}

static void write_table(const char* path) {
    unsigned long version = header_version();
    unsigned index = 0;
    out("/* Generated by tools/vkcmdgen from the Khronos registry, vk.xml of VK_HEADER_VERSION %lu. Do not edit. */\n"
        "#include \"common/gpu/vk/extension_commands.h\"\n\n"
        "const SnxVkExtensionCommand snx_vk_extension_commands[] = {\n",
        version);
    /* in the sorted extensions' order: each one's first is where its commands start here */
    for (unsigned i = 0; i < g_vkcmdgen.nextensions; i++) {
        Extension* e = &g_vkcmdgen.extensions[i];
        unsigned first = index;
        for (unsigned k = 0; k < e->count; k++, index++) {
            const Required* r = &g_vkcmdgen.required[e->first + k];
            if (r->depends_n)
                out("    {\"%.*s\", \"%.*s\"},\n", (int)r->name_n, at_(r->name), (int)r->depends_n, at_(r->depends));
            else
                out("    {\"%.*s\", NULL},\n", (int)r->name_n, at_(r->name));
        }
        e->first = first;
    }
    if (!index) out("    {NULL, NULL},\n");
    out("};\n\nconst SnxVkExtensionEntry snx_vk_extension_table[] = {\n");
    for (unsigned i = 0; i < g_vkcmdgen.nextensions; i++) {
        const Extension* e = &g_vkcmdgen.extensions[i];
        out("    {\"%.*s\", %u, %u},\n", (int)e->name_n, at_(e->name), e->first, e->count);
    }
    out("};\nconst uint32_t snx_vk_extension_table_count = %u;\n", g_vkcmdgen.nextensions);

    FILE* f = fopen(path, "rb");
    if (f) {
        char* old = (char*)malloc(g_vkcmdgen.out_n + 1);
        size_t got = old ? fread(old, 1, g_vkcmdgen.out_n + 1, f) : 0;
        int same = old && got == g_vkcmdgen.out_n && !memcmp(old, g_vkcmdgen.out, got);
        free(old);
        fclose(f);
        if (same) return;
    }
    f = fopen(path, "wb");
    if (!f || fwrite(g_vkcmdgen.out, 1, g_vkcmdgen.out_n, f) != g_vkcmdgen.out_n || fclose(f))
        fail(0, "cannot write %s", path);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: vkcmdgen REGISTRY OUT.c\n");
        return 2;
    }
    g_vkcmdgen.path = argv[1];
    g_vkcmdgen.text = read_file(argv[1]);
    read_handles();
    read_commands();
    read_extensions();
    write_table(argv[2]);
    free(g_vkcmdgen.text);
    free(g_vkcmdgen.out);
    return 0;
}
