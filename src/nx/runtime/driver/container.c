/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Switch dumps through hactool (hactool/snx_hac.h): the keys from their files into hactool's keyset in memory, the
   title keys of the dump's tickets, and the NCAs a game is made from */
#include "nx/runtime/driver/container.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name, *value;
    size_t name_size, value_size;
} KeyLine;

static char* text_copy(const char* text, size_t size) {
    char* copy;
    if (size == SIZE_MAX) return NULL;
    copy = (char*)malloc(size + 1);
    if (copy) {
        memcpy(copy, text, size);
        copy[size] = 0;
    }
    return copy;
}

static int ascii_name(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int ascii_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int has_suffix(const char* text, const char* suffix) {
    size_t a, b, i;
    a = strlen(text);
    b = strlen(suffix);
    if (a < b) return 0;
    for (i = 0; i < b; i++)
        if (ascii_lower(text[a - b + i]) != ascii_lower(suffix[i])) return 0;
    return 1;
}

static SnxU64 le_bytes(const unsigned char* p, unsigned n) {
    SnxU64 value = 0;
    while (n--) value = value << 8 | p[n];
    return value;
}

/* -1 when path can't be read, else 0 for an NSP, 1 for an XCI, 2 for neither. A gamecard dump can start with the
   card's 0x1000-byte key area, its header after it (LibHac's XciHeader) */
static int kind_of(const char* path) {
    unsigned char header[0x1104];
    size_t size;
    FILE* file;
    int failed;
    file = snx_fopen(path, "rb");
    if (!file) return -1;
    size = fread(header, 1, sizeof header, file);
    failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) return -1;
    if (size >= 4 && !memcmp(header, "PFS0", 4)) return 0;
    if ((size >= 0x104 && (!memcmp(header + 0x100, "HEAD", 4) || !memcmp(header + 0x100, "DXCI", 4))) ||
        (size == sizeof header && (!memcmp(header + 0x1100, "HEAD", 4) || !memcmp(header + 0x1100, "DXCI", 4))))
        return 1;
    return 2;
}

int snx_container_kind(const char* path, int* xci) {
    char why[256];
    int kind = kind_of(path);
    if (kind < 0) return snx_error("cannot read %s: %s", path, snx_os_error(why, sizeof why));
    if (kind == 2) return snx_error("%s is not an NSP/DNSP (PFS0) or an XCI/DXCI (HEAD/DXCI)", path);
    *xci = kind;
    return 0;
}

int snx_is_dump(const char* path) {
    int kind = kind_of(path);
    return kind == 0 || kind == 1;
}

char* snx_find_keys(const char* given, const char* name, const char* repo) {
    const char* home;
    char *in_repo, *directory, *in_home, *absolute;
    if (given) return snx_absolute(given);
#ifdef SNX_IOS
    { /* the data folder keeps the keys in keys/ (snx_keys_dir) */
        char* keys = repo ? snx_path_join(repo, "keys") : NULL;
        in_repo = keys ? snx_path_join(keys, name) : NULL;
        free(keys);
    }
#else
    in_repo = repo ? snx_path_join(repo, name) : NULL;
#endif
    if (in_repo && snx_is_file(in_repo)) return in_repo;
#if defined(_WIN32)
    home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home || !home[0]) return in_repo ? in_repo : snx_strdup(name);
    directory = snx_path_join(home, ".switch");
    in_home = directory ? snx_path_join(directory, name) : NULL;
    free(directory);
    if (!in_home) {
        free(in_repo);
        return NULL;
    }
    if (!snx_is_file(in_home)) {
        if (!in_repo) return in_home;
        free(in_home);
        return in_repo;
    }
    absolute = snx_absolute(in_home);
    free(in_home);
    free(in_repo);
    return absolute;
}

static int key_line(const char* text, size_t size, KeyLine* line) {
    size_t i, start;
    i = 0;
    while (i < size && isspace((unsigned char)text[i])) i++;
    start = i;
    while (i < size && ascii_name((unsigned char)text[i])) i++;
    if (i == start) return 0;
    line->name = text + start;
    line->name_size = i - start;
    while (i < size && isspace((unsigned char)text[i])) i++;
    if (i == size || text[i++] != '=') return 0;
    while (i < size && isspace((unsigned char)text[i])) i++;
    start = i;
    while (i < size && ascii_hex((unsigned char)text[i])) i++;
    if (i == start) return 0;
    line->value = text + start;
    line->value_size = i - start;
    while (i < size && isspace((unsigned char)text[i])) i++;
    return i == size;
}

static void secret_free(void* memory, size_t size) {
    volatile unsigned char* p;
    p = (volatile unsigned char*)memory;
    if (!p) return;
    while (size) {
        *p++ = 0;
        size--;
    }
    free(memory);
}

int snx_hac_error(const char* what) {
    const char* said = snx_hac_message();
    return said[0] ? snx_error("%s (hactool: %s)", what, said) : snx_error("%s", what);
}
/* every NCA's header unreadable: the prod.keys that has no right header_key named */
static int headers_unreadable(const SnxDump* dump) {
    char what[1200];
    snprintf(what, sizeof what, "the NCAs' headers cannot be decrypted: %s's header_key is missing or wrong",
             dump->keys ? dump->keys : "prod.keys");
    return snx_hac_error(what);
}

static int hex_bytes(const char* text, size_t size, unsigned char* out) {
    size_t i;
    unsigned digit;
    for (i = 0; i < size; i++) {
        if (!ascii_hex((unsigned char)text[i])) return -1;
        digit = text[i] <= '9' ? (unsigned)(text[i] - '0') : (unsigned)(ascii_lower(text[i]) - 'a' + 10);
        if (i & 1)
            out[i / 2] = (unsigned char)(out[i / 2] | digit);
        else
            out[i / 2] = (unsigned char)(digit << 4);
    }
    return 0;
}

void snx_size_text(SnxU64 bytes, char* text, size_t size) {
    static const char* const units[] = {"KB", "MB", "GB", "TB"};
    double value = (double)bytes;
    int unit = -1;
    if (bytes < 1024) {
        sprintf(text, "%llu bytes", (unsigned long long)bytes);
        return;
    }
    while (value >= 1024 && unit < 3) {
        value /= 1024;
        unit++;
    }
    if (size >= 16) sprintf(text, value < 10 ? "%.1f %s" : "%.0f %s", value, units[unit]);
}
/* the firmware whose master key is generation's, the first that reads content made with it (Switchbrew's
   "Cryptosystem"), NULL for one newer than this table */
static const char* firmware_of(int generation) {
    static const char* const firmware[] = {"1.0.0",  "3.0.0",  "3.0.1",  "4.0.0",  "5.0.0",  "6.0.0",  "6.2.0",
                                           "7.0.0",  "8.1.0",  "9.0.0",  "9.1.0",  "12.1.0", "13.0.0", "14.0.0",
                                           "15.0.0", "16.0.0", "17.0.0", "18.0.0", "19.0.0", "20.0.0"};
    return generation >= 0 && generation < (int)(sizeof firmware / sizeof *firmware) ? firmware[generation] : NULL;
}
static const char* content_name(int content) {
    static const char* const names[] = {"Program", "Meta", "Control", "Manual", "Data", "PublicData"};
    return content >= 0 && content < 6 ? names[content] : "unknown";
}
/* "master key 16 (firmware 17.0.0 and later)" into text */
static void generation_text(int generation, char* text) {
    const char* firmware = firmware_of(generation);
    if (firmware)
        sprintf(text, "master key %d (firmware %s and later)", generation, firmware);
    else
        sprintf(text, "master key %d (a firmware newer than %s)", generation, firmware_of(19));
}
/* prod.keys into hactool's keyset. A value of a length no key has is left out, as hactool would stop on it */
static int load_prod_keys(SnxDump* dump, int say) {
    unsigned char* data;
    size_t size, start, end;
    unsigned long dropped, known = 0, line_number = 0, first_dropped = 0;
    char *name, *value;
    KeyLine line;
    int failed;
    char why[256];
    data = snx_read_file(dump->keys, &size);
    if (!data) return snx_error("cannot read %s: %s", dump->keys, snx_os_error(why, sizeof why));
    start = 0;
    dropped = 0;
    failed = 0;
    while (start < size && !failed) {
        end = start;
        while (end < size && data[end] != '\n') end++;
        ++line_number;
        if (key_line((const char*)data + start, end - start, &line)) {
            if (line.value_size % 32) {
                if (!dropped++) first_dropped = line_number;
            } else {
                name = text_copy(line.name, line.name_size);
                value = text_copy(line.value, line.value_size);
                if (!name || !value)
                    failed = snx_error("out of memory reading prod.keys");
                else
                    known += snx_hac_set_key(dump->hac, name, value);
                if (name) secret_free(name, line.name_size);
                if (value) secret_free(value, line.value_size);
            }
        }
        start = end < size ? end + 1 : end;
    }
    secret_free(data, size);
    if (failed) return 1;
    snx_hac_derive_keys(dump->hac);
    if (dropped == 1 && say) printf("%s: line %lu is not a key\n", dump->keys, first_dropped);
    if (dropped > 1 && say)
        printf("%s: %lu lines are not keys, the first line %lu\n", dump->keys, dropped, first_dropped);
    if (say) { /* where they are and how many, never one of them */
        int newest = snx_hac_newest_generation(dump->hac);
        char text[96];
        printf("Keys: %s, %lu of them", dump->keys, known);
        if (newest >= 0) {
            generation_text(newest, text);
            printf(", up to %s", text);
        }
        puts("");
    }
    return 0;
}

/* title.keys, when there is one: a rights ID and its title key a line */
static int load_title_keys(SnxDump* dump) {
    unsigned char *data, rights[16], key[16];
    char why[256];
    size_t size, start, end;
    KeyLine line;
    unsigned long given = 0;
    if (!snx_is_file(dump->title_keys)) return 0;
    data = snx_read_file(dump->title_keys, &size);
    if (!data) return snx_error("cannot read %s: %s", dump->title_keys, snx_os_error(why, sizeof why));
    start = 0;
    while (start < size) {
        end = start;
        while (end < size && data[end] != '\n') end++;
        if (key_line((const char*)data + start, end - start, &line) && line.name_size == 32 && line.value_size == 32 &&
            !hex_bytes(line.name, 32, rights) && !hex_bytes(line.value, 32, key)) {
            snx_hac_add_title_key(dump->hac, rights, key);
            given++;
        }
        start = end < size ? end + 1 : end;
    }
    memset(key, 0, sizeof key);
    secret_free(data, size);
    if (dump->say) printf("Title keys: %s, %lu of them\n", dump->title_keys, given);
    return 0;
}

/* the title keys of a container's tickets: common tickets, whose key is stored as is */
/* the file of container, as a message names it */
static const char* dump_file(const SnxDump* dump, int container) {
    return container >= 0 && container < (int)(sizeof dump->opened / sizeof *dump->opened) && dump->opened[container]
               ? dump->opened[container]
               : "the dump";
}
static int read_tickets(SnxDump* dump, int container, unsigned* tickets, unsigned* personal) {
    unsigned char data[1024];
    const SnxHacEntry* entries;
    size_t count, i, size, offset;
    count = snx_hac_entries(dump->hac, container, &entries);
    *tickets = *personal = 0;
    for (i = 0; i < count; i++) {
        if (!has_suffix(entries[i].name, ".tik")) continue;
        ++*tickets;
        size = entries[i].size < sizeof data ? (size_t)entries[i].size : sizeof data;
        if (snx_hac_read(dump->hac, container, i, data, size))
            return snx_error("cannot read the ticket %s", entries[i].name);
        if (size < 4) continue;
        switch ((SnxU32)le_bytes(data, 4)) {
            case 0x10000UL:
            case 0x10003UL: offset = 4 + 0x200 + 0x3c; break;
            case 0x10001UL:
            case 0x10004UL: offset = 4 + 0x100 + 0x3c; break;
            case 0x10002UL:
            case 0x10005UL: offset = 4 + 0x3c + 0x40; break;
            case 0x10006UL: offset = 4 + 0x14 + 0x28; break;
            default: continue;
        }
        if (size >= offset + 0x170 && data[offset + 0x141]) ++*personal;
        if (size < offset + 0x170 || data[offset + 0x141]) continue; /* a personalized ticket's key is encrypted */
        snx_hac_add_title_key(dump->hac, data + offset + 0x160, data + offset + 0x40);
    }
    memset(data, 0, sizeof data);
    return 0;
}

/* 1 when an NCA's header is encrypted: a decrypted one (DNCA) or a plain NCA3 or NCA2 header needs no keys */
static int nca_needs_keys(SnxDump* dump, int container, size_t entry) {
    unsigned char header[0x204];
    if (snx_hac_read(dump->hac, container, entry, header, sizeof header)) return 1;
    return memcmp(header + 0x200, "DNCA", 4) && memcmp(header + 0x200, "NCA3", 4) && memcmp(header + 0x200, "NCA2", 4);
}

static void take(SnxNcaRef* ref, int container, size_t entry, const SnxHacEntry* e, const SnxHacNca* nca) {
    ref->found = 1;
    ref->container = container;
    ref->entry = entry;
    ref->size = e->size;
    ref->title_id = nca->title_id;
    ref->info = *nca;
}

/* a line saying what an NCA is and what reading it takes */
static void say_nca(const SnxHacNca* nca, const SnxHacEntry* entry) {
    char size[32], title[17], keys[96];
    snx_size_text(entry->size, size, sizeof size);
    snx_title_text(nca->title_id, title);
    printf("  %s NCA of %s, %s", content_name(nca->content), title[0] ? title : "no title", size);
    if (nca->sdk)
        printf(", SDK %u.%u.%u", (unsigned)(nca->sdk >> 24), (unsigned)(nca->sdk >> 16 & 0xFF),
               (unsigned)(nca->sdk >> 8 & 0xFF));
    if (nca->patch) printf(", an update's patch");
    if (nca->decrypted)
        printf(", decrypted\n");
    else {
        generation_text(nca->generation, keys);
        printf(", %s, %s%s\n", keys,
               nca->title_key > 0   ? "its title key from a ticket or title.keys"
               : nca->title_key < 0 ? "its title key in no ticket of the dump or title.keys"
                                    : "its keys in its key area",
               nca->keys ? "" : ", and prod.keys has no key of that master key");
    }
}
int snx_nca_keys(const SnxDump* dump, const SnxNcaRef* nca, const char* what) {
    char needs[96], newest[96];
    if (nca->info.decrypted) return 0;
    generation_text(nca->info.generation, needs);
    if (!nca->info.keys && nca->info.newest < 0)
        return snx_error("%s is encrypted with %s, and %s has the keys of no master key: it needs the keys of a "
                         "console on that firmware or a later one",
                         what, needs, dump->keys);
    if (!nca->info.keys) {
        generation_text(nca->info.newest, newest);
        return snx_error("%s is encrypted with %s, and %s goes up to %s: it needs the keys of a console with master "
                         "key %d, on its firmware or a later one",
                         what, needs, dump->keys, newest, nca->info.generation);
    }
    if (nca->info.title_key < 0)
        return snx_error("%s needs a title key that no ticket in the dump and no line of %s has: a personalized "
                         "ticket's key is its console's, so dump that console's title.keys",
                         what, dump->title_keys);
    return 0;
}

/* the headers of a container's NCAs, the content meta's left out (SNX_HAC_OTHER), into *ncas (owned, count of them
   as there are entries): 0, else nonzero, said why. One whose header cannot be decrypted counts in *unreadable */
static int read_ncas(SnxDump* dump, int container, const SnxHacEntry** entries, size_t* count, SnxHacNca** ncas,
                     unsigned long* unreadable) {
    size_t i;
    *count = snx_hac_entries(dump->hac, container, entries);
    *ncas = (SnxHacNca*)calloc(*count ? *count : 1, sizeof **ncas);
    *unreadable = 0;
    if (!*ncas) return snx_error("out of memory reading the NCAs' headers");
    for (i = 0; i < *count; i++) {
        (*ncas)[i].type = SNX_HAC_OTHER;
        if (!has_suffix((*entries)[i].name, ".nca") || has_suffix((*entries)[i].name, ".cnmt.nca")) continue;
        if (!dump->has_keys && nca_needs_keys(dump, container, i)) {
            free(*ncas);
            *ncas = NULL;
            return snx_error("encrypted content needs prod.keys. Provide --keys, or put it at %s or in ~/.switch",
                             dump->keys);
        }
        if (snx_hac_nca(dump->hac, container, i, &(*ncas)[i])) {
            (*ncas)[i].type = SNX_HAC_OTHER;
            ++*unreadable;
        } else if (dump->say && !(dump->said & 1u << container))
            say_nca(&(*ncas)[i], &(*entries)[i]);
    }
    if (*unreadable && dump->say && !(dump->said & 1u << container))
        printf("  %u NCAs whose headers cannot be decrypted: prod.keys' header_key is missing or wrong\n",
               (unsigned)*unreadable);
    if (dump->say && !(dump->said & 1u << container)) fflush(stdout);
    dump->said |= 1u << container;
    return 0;
}

/* A content meta (CNMT, Switchbrew's "CNMT"): what the console installs a title by, in the .cnmt file of a Meta NCA. Its
   header is 0x20 bytes: the title ID, the version, the type, the extended header's size, the contents' count. An
   update's and add-on content's extended header starts with the ID of the application they belong to. Each content
   record after it is 0x38 bytes: a hash, then the content ID (the NCA's file name before .nca), its size, its type
   and, for a program, its index in the application */
enum { META_APPLICATION = 0x80, META_PATCH = 0x81, META_ADDON = 0x82 };
enum { CONTENT_PROGRAM = 1, CONTENT_DATA = 2, CONTENT_CONTROL = 3, CONTENT_HTML_DOCUMENT = 4 };
typedef struct {
    unsigned char id[16];
    int type, index;
} MetaContent;
typedef struct {
    int type;
    SnxU64 title, application; /* an application's own ID for one */
    SnxU32 version;
    size_t count;
    MetaContent* contents; /* owned */
} Meta;

/* d (size bytes) into m: 0, or -1 for no content meta of the types read here */
static int meta_parse(const unsigned char* d, size_t size, Meta* m) {
    size_t extended, records, i;
    memset(m, 0, sizeof *m);
    if (size < 0x20) return -1;
    m->title = le_bytes(d, 8);
    m->version = (SnxU32)le_bytes(d + 8, 4);
    m->type = d[0xC];
    extended = (size_t)le_bytes(d + 0xE, 2);
    records = (size_t)le_bytes(d + 0x10, 2);
    if (m->type != META_APPLICATION && m->type != META_PATCH && m->type != META_ADDON) return -1;
    if (m->type != META_APPLICATION && extended < 8) return -1;
    if (0x20 + extended > size || records > (size - 0x20 - extended) / 0x38) return -1;
    m->application = m->type == META_APPLICATION ? m->title : le_bytes(d + 0x20, 8);
    m->contents = (MetaContent*)calloc(records ? records : 1, sizeof *m->contents);
    if (!m->contents) return -1;
    for (i = 0; i < records; i++) {
        const unsigned char* r = d + 0x20 + extended + i * 0x38;
        memcpy(m->contents[i].id, r + 0x20, 16);
        m->contents[i].type = r[0x36];
        m->contents[i].index = r[0x37];
    }
    m->count = records;
    return 0;
}
static void metas_free(Meta* metas, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) free(metas[i].contents);
    free(metas);
}
static const char* meta_name(int type) {
    return type == META_APPLICATION ? "an application" : type == META_PATCH ? "an update" : "add-on content (a DLC)";
}
/* the content metas of a container's Meta NCAs that can be read, into *metas (owned): their count, said once */
static size_t read_metas(SnxDump* dump, int container, Meta** metas) {
    const SnxHacEntry* entries;
    size_t count = snx_hac_entries(dump->hac, container, &entries), i, n = 0, size;
    unsigned char* data;
    char title[17], application[17];
    *metas = (Meta*)calloc(count ? count : 1, sizeof **metas);
    for (i = 0; *metas && i < count; i++) {
        SnxHacNca nca;
        /* a Meta NCA by its header first: hactool ends the process on one it cannot process */
        if (!has_suffix(entries[i].name, ".cnmt.nca") || entries[i].size < 0xC00 ||
            (!dump->has_keys && nca_needs_keys(dump, container, i)) || snx_hac_nca(dump->hac, container, i, &nca) ||
            nca.content != 1 || (!nca.decrypted && !nca.keys) ||
            snx_hac_cnmt(dump->hac, container, i, &data, &size))
            continue;
        if (!meta_parse(data, size, &(*metas)[n])) {
            if (dump->say && !(dump->said_meta & 1u << container)) {
                const Meta* m = &(*metas)[n];
                snx_title_text(m->title, title);
                snx_title_text(m->application, application);
                printf("  content meta: %s %s, version %lu", meta_name(m->type), title, (unsigned long)m->version);
                if (m->type != META_APPLICATION) printf(", of the game %s", application);
                puts("");
            }
            n++;
        }
        free(data);
    }
    for (i = 0, size = 0; i < count; i++) size += has_suffix(entries[i].name, ".nca"); /* any NCA to tell them by */
    if (dump->say && !(dump->said_meta & 1u << container) && !n && size)
        puts("  no content meta that can be read: its kind and game told from its NCAs' title IDs");
    dump->said_meta |= 1u << container;
    return n;
}
/* the entry of a content the meta lists, of type (and for a program, index 0, the application's main one): its index
   in entries, or -1 when the container lacks it. An NCA is named by its content ID in hex, then .nca */
static long meta_entry(const Meta* m, int type, const SnxHacEntry* entries, size_t count) {
    static const char digits[] = "0123456789abcdef";
    char name[33];
    size_t i, j, k;
    for (i = 0; i < m->count; i++) {
        if (m->contents[i].type != type || (type == CONTENT_PROGRAM && m->contents[i].index)) continue;
        for (k = 0; k < 16; k++) {
            name[2 * k] = digits[m->contents[i].id[k] >> 4];
            name[2 * k + 1] = digits[m->contents[i].id[k] & 15];
        }
        name[32] = 0;
        for (j = 0; j < count; j++)
            if (!strncmp(entries[j].name, name, 32) && entries[j].name[32] == '.' && has_suffix(entries[j].name, ".nca"))
                return (long)j;
    }
    return -1;
}
/* ref taken from the meta's content of type, its title the meta's: 1, else 0 when the container lacks it */
static int take_content(SnxNcaRef* ref, const Meta* m, int type, int container, const SnxHacEntry* entries,
                        const SnxHacNca* ncas, size_t count) {
    long at = meta_entry(m, type, entries, count);
    if (at < 0) return 0;
    take(ref, container, (size_t)at, &entries[at], &ncas[at]);
    ref->title_id = m->title; /* the meta's, whatever the NCA's header says */
    return 1;
}

/* an update's Program NCA told from its header, without a content meta: its title ID with 0x800, or a Patch RomFS */
static int is_patch(const SnxHacNca* nca) {
    return nca->type == SNX_HAC_PROGRAM && ((nca->title_id & 0x800) || nca->patch);
}

/* the base's Program NCA of a container (selected), the patch an NSP of both has (embedded_patch), and the Control
   NCA of the same title (control): the contents its application's content meta lists, else told from the NCAs'
   headers, the biggest when there are several. dump->game is the application's ID */
static int select_ncas(SnxDump* dump, int container, SnxNcaRef* selected, SnxNcaRef* embedded_patch,
                       SnxNcaRef* control) {
    const SnxHacEntry* entries;
    SnxHacNca* ncas;
    Meta* metas = NULL;
    size_t count, i, nmetas;
    unsigned long unreadable;
    const SnxU64 update_bit = 0x800UL;
    memset(selected, 0, sizeof *selected);
    if (embedded_patch) memset(embedded_patch, 0, sizeof *embedded_patch);
    memset(control, 0, sizeof *control);
    if (read_ncas(dump, container, &entries, &count, &ncas, &unreadable)) return 1;
    nmetas = read_metas(dump, container, &metas);
    for (i = 0; i < nmetas && !selected->found; i++)
        if (metas[i].type == META_APPLICATION &&
            take_content(selected, &metas[i], CONTENT_PROGRAM, container, entries, ncas, count)) {
            take_content(control, &metas[i], CONTENT_CONTROL, container, entries, ncas, count);
            take_content(&dump->html, &metas[i], CONTENT_HTML_DOCUMENT, container, entries, ncas, count);
        }
    for (i = 0; embedded_patch && selected->found && i < nmetas && !embedded_patch->found; i++)
        if (metas[i].type == META_PATCH && metas[i].application == selected->title_id &&
            take_content(embedded_patch, &metas[i], CONTENT_PROGRAM, container, entries, ncas, count)) {
            take_content(control, &metas[i], CONTENT_CONTROL, container, entries, ncas, count); /* a name changed */
            take_content(&dump->html, &metas[i], CONTENT_HTML_DOCUMENT, container, entries, ncas, count);
        }
    if (!selected->found) { /* no content meta naming them */
        for (i = 0; i < count; i++)
            if (ncas[i].type == SNX_HAC_PROGRAM && !is_patch(&ncas[i]) &&
                (!selected->found || entries[i].size > selected->size))
                take(selected, container, i, &entries[i], &ncas[i]);
        for (i = 0; selected->found && i < count; i++) {
            if (embedded_patch && is_patch(&ncas[i]) && (ncas[i].title_id & ~update_bit) == selected->title_id &&
                (!embedded_patch->found || entries[i].size > embedded_patch->size))
                take(embedded_patch, container, i, &entries[i], &ncas[i]);
            if (!control->found && ncas[i].type == SNX_HAC_CONTROL && /* the same game's, base or update */
                (ncas[i].title_id & ~update_bit) == (selected->title_id & ~update_bit))
                take(control, container, i, &entries[i], &ncas[i]);
        }
    }
    free(ncas);
    metas_free(metas, nmetas);
    dump->game = selected->title_id;
    if (selected->found) return 0;
    if (unreadable) return headers_unreadable(dump);
    return snx_error("%s has no base Program NCA: an update's or a DLC's dump goes with --update, after the game's",
                     dump_file(dump, container));
}

/* An add-on's container: an update (its Program NCA, the patch, with its Control NCA) or else a DLC (its Data NCA), as
   its content meta lists them, the game it belongs to the one the meta names (dump->game). With the base opened
   (program found), only the base's own add-ons. Without a content meta, told from the NCAs' headers: the game is the
   one the usual numbering gives (snx_addon_guess), which a DLC numbered otherwise has no other way to be told by. The
   biggest when there are several */
static int select_addon(SnxDump* dump, int container) {
    const SnxHacEntry* entries;
    SnxHacNca* ncas;
    Meta* metas = NULL;
    size_t count, i, nmetas, dlcs = 0;
    unsigned long unreadable;
    SnxU64 game = dump->program.found ? dump->game : 0, other = 0;
    int patches = 0; /* another game's */
    memset(&dump->patch, 0, sizeof dump->patch);
    memset(&dump->aoc, 0, sizeof dump->aoc);
    dump->version = SNX_NO_VERSION;
    if (read_ncas(dump, container, &entries, &count, &ncas, &unreadable)) return 1;
    nmetas = read_metas(dump, container, &metas);
    for (i = 0; i < nmetas && !dump->patch.found; i++) {
        if (metas[i].type != META_PATCH) continue;
        if (game && metas[i].application != game) {
            other = metas[i].application;
            continue;
        }
        if (take_content(&dump->patch, &metas[i], CONTENT_PROGRAM, container, entries, ncas, count)) {
            take_content(&dump->control, &metas[i], CONTENT_CONTROL, container, entries, ncas, count);
            take_content(&dump->html, &metas[i], CONTENT_HTML_DOCUMENT, container, entries, ncas, count);
            dump->game = metas[i].application;
            dump->version = metas[i].version;
        }
    }
    for (i = 0; i < nmetas && !dump->patch.found; i++) {
        if (metas[i].type != META_ADDON) continue;
        if (game && metas[i].application != game) {
            other = metas[i].application;
            continue;
        }
        if (dump->aoc.found)
            dlcs++;
        else if (take_content(&dump->aoc, &metas[i], CONTENT_DATA, container, entries, ncas, count)) {
            dump->game = metas[i].application;
            dump->version = metas[i].version;
            dlcs++;
        }
    }
    if (dlcs > 1 && dump->say)
        printf("  %lu DLCs in one dump: the first is used, give the others' dumps each\n", (unsigned long)dlcs);
    if (!nmetas) { /* told from the NCAs' headers */
        for (i = 0; i < count; i++) {
            SnxU64 own = game ? game : snx_addon_guess(ncas[i].title_id);
            patches += is_patch(&ncas[i]);
            if (is_patch(&ncas[i]) && (ncas[i].title_id & ~(SnxU64)0x800) == own &&
                (!dump->patch.found || (ncas[i].title_id == dump->patch.title_id && entries[i].size > dump->patch.size))) {
                take(&dump->patch, container, i, &entries[i], &ncas[i]);
                dump->game = own;
            }
        }
        for (i = 0; i < count; i++) {
            if (dump->patch.found && ncas[i].type == SNX_HAC_CONTROL &&
                (ncas[i].title_id & ~(SnxU64)0x800) == dump->game)
                take(&dump->control, container, i, &entries[i], &ncas[i]); /* the update's, a name changed in it */
            else if (!dump->patch.found && ncas[i].type == SNX_HAC_DATA &&
                     snx_addon_kind(snx_addon_guess(ncas[i].title_id), ncas[i].title_id) == SNX_ADDON_DLC &&
                     (!game || snx_addon_guess(ncas[i].title_id) == game) &&
                     (!dump->aoc.found || (ncas[i].title_id == dump->aoc.title_id && entries[i].size > dump->aoc.size))) {
                take(&dump->aoc, container, i, &entries[i], &ncas[i]);
                dump->game = snx_addon_guess(ncas[i].title_id);
            }
        }
    }
    free(ncas);
    metas_free(metas, nmetas);
    if (dump->patch.found || dump->aoc.found) return 0;
    if (unreadable) return headers_unreadable(dump);
    if (other) {
        char of[17], given[17];
        snx_title_text(other, of);
        snx_title_text(game, given);
        return snx_error("the update or DLC belongs to the game %s, not to the selected base game %s", of, given);
    }
    {
        char given[17];
        const char* dump_path = dump_file(dump, container);
        snx_title_text(game, given);
        if (game && patches) return snx_error("%s has no Program NCA of the game %s", dump_path, given);
        if (game) return snx_error("%s is not a DLC of the game %s", dump_path, given);
        return snx_error("%s has no base Program NCA, and no update or DLC either: it is no game's dump", dump_path);
    }
}

static int open_container(SnxDump* dump, const char* path, int say) {
    int xci, container;
    unsigned tickets, personal;
    const SnxHacEntry* entries;
    size_t count, i, ncas = 0;
    char size[32];
    if (snx_container_kind(path, &xci)) return -1;
    if (say) {
        printf("Reading %s\n", path);
        fflush(stdout);
    }
    container = snx_hac_open(dump->hac, path, xci);
    if (container >= 0 && container < (int)(sizeof dump->opened / sizeof *dump->opened)) dump->opened[container] = path;
    if (container < 0) snx_hac_error(xci ? "cannot read the XCI's secure partition" : "cannot read the NSP");
    if (container < 0 || read_tickets(dump, container, &tickets, &personal)) return -1;
    if (say) { /* what it holds: its NCAs, said each as they are read, and its tickets */
        count = snx_hac_entries(dump->hac, container, &entries);
        for (i = 0; i < count; i++) /* the content meta's left out, as the reading leaves it */
            ncas += has_suffix(entries[i].name, ".nca") && !has_suffix(entries[i].name, ".cnmt.nca");
        snx_size_text(snx_file_size(path), size, sizeof size);
        printf("  %s, %s: %lu NCA%s", xci ? "a gamecard (XCI)" : "an NSP", size, (unsigned long)ncas, ncas == 1 ? "" : "s");
        if (tickets)
            printf(", %u ticket%s%s", tickets, tickets == 1 ? "" : "s",
                   personal == tickets ? " (personalized: their title keys are encrypted for a console)"
                   : personal          ? " (some personalized)"
                                       : "");
        printf("%s\n", dump->has_keys ? "" : ", read without prod.keys");
    }
    return container;
}

/* the keys loaded into a new hactool: 0, else nonzero, said why */
static int dump_start(SnxDump* dump, const char* keys, const char* title_keys, const char* repo, int say) {
    memset(dump, 0, sizeof *dump);
    dump->base = dump->update = -1;
    dump->say = say;
    dump->version = SNX_NO_VERSION;
    dump->keys = snx_find_keys(keys, "prod.keys", repo);
    dump->title_keys = snx_find_keys(title_keys, "title.keys", repo);
    dump->hac = snx_hac_new();
    if (!dump->keys || !dump->title_keys || !dump->hac) return snx_error("out of memory reading the dump");
    dump->has_keys = snx_is_file(dump->keys);
    if (keys && !dump->has_keys) return snx_error("%s is not a file", dump->keys);
    return (dump->has_keys && load_prod_keys(dump, say)) || load_title_keys(dump);
}

int snx_dump_open(SnxDump* dump, const char* base, const char* addon, const char* keys, const char* title_keys,
                  const char* repo, int say) {
    if (dump_start(dump, keys, title_keys, repo, say)) return 1;
    /* each container's NCAs read as it is opened, the base's before the add-on's is */
    if (base && ((dump->base = open_container(dump, base, say)) < 0 ||
                 select_ncas(dump, dump->base, &dump->program, addon ? NULL : &dump->patch, &dump->control)))
        return 1;
    if (addon && (dump->update = open_container(dump, addon, say)) < 0) return 1;
    return addon ? select_addon(dump, dump->update) : 0;
}

int snx_dump_title(SnxDump* dump, const char* path, const char* keys, const char* title_keys, const char* repo, int say,
                   SnxU64* title, SnxU64* game, SnxU64* version) {
    const SnxHacEntry* entries;
    SnxHacNca* ncas;
    Meta* metas = NULL;
    size_t count, i, nmetas;
    unsigned long unreadable;
    int application = 0;
    if (dump_start(dump, keys, title_keys, repo, say) || (dump->base = open_container(dump, path, say)) < 0 ||
        read_ncas(dump, dump->base, &entries, &count, &ncas, &unreadable))
        return 1;
    /* a game's dump when its content meta says so, else when it has a game's program; an update's or a DLC's else */
    nmetas = read_metas(dump, dump->base, &metas);
    for (i = 0; i < nmetas; i++) application |= metas[i].type == META_APPLICATION;
    for (i = 0; !nmetas && i < count && !application; i++)
        application = ncas[i].type == SNX_HAC_PROGRAM && !is_patch(&ncas[i]);
    free(ncas);
    metas_free(metas, nmetas);
    if (!application) {
        dump->update = dump->base;
        dump->base = -1;
        if (select_addon(dump, dump->update)) return 1;
        *title = dump->patch.found ? dump->patch.title_id : dump->aoc.title_id;
        *game = dump->game;
        if (version) *version = dump->version;
        return 0;
    }
    if (select_ncas(dump, dump->base, &dump->program, &dump->patch, &dump->control)) return 1;
    *title = *game = dump->program.title_id;
    if (version) *version = SNX_NO_VERSION;
    return 0;
}

void snx_dump_close(SnxDump* dump) {
    snx_hac_free(dump->hac);
    free(dump->keys);
    free(dump->title_keys);
    memset(dump, 0, sizeof *dump);
}

SnxHacFs* snx_dump_fs(SnxDump* dump) {
    const SnxNcaRef* top = dump->patch.found ? &dump->patch : &dump->program;
    int under = dump->patch.found && dump->program.found;
    SnxHacFs* fs = snx_hac_fs_open(dump->hac, top->container, top->entry, under ? dump->program.container : -1,
                                   dump->program.entry);
    if (!fs) snx_hac_error("the Program NCA cannot be read");
    return fs;
}

SnxHacFs* snx_dump_html_fs(SnxDump* dump) {
    if (!dump->html.found) return NULL;
    return snx_hac_fs_open(dump->hac, dump->html.container, dump->html.entry, -1, 0);
}

SnxHacFs* snx_dump_aoc_fs(SnxDump* dump) {
    SnxHacFs* fs = snx_hac_fs_open(dump->hac, dump->aoc.container, dump->aoc.entry, -1, 0);
    if (!fs) snx_hac_error("the DLC's Data NCA cannot be read");
    return fs;
}

/* The RomFS's header and tables (Switchbrew's "RomFS"): ten little-endian 64-bit fields, the root's directory entry
   first in its table, a directory entry its parent, sibling, first child directory and first file, then its name
   (0x18 on), a file entry its parent, sibling, offset and size, then its name (0x20 on). Each directory of name's
   path (dir/dir/file) is found among its parent's children, and the file among the last one's files */
unsigned char* snx_romfs_file(SnxHacFs* fs, const char* name, size_t* size) {
    unsigned char header[0x50], entry[0x20 + 0x300], *data;
    SnxU64 total = snx_hac_fs_romfs_size(fs), table, table_size, files, start, length, dirs, dirs_size;
    SnxU32 at, name_size, dir = 0;
    size_t want;
    const char* slash;
    unsigned visited;
    if (total < sizeof header || snx_hac_fs_read_romfs(fs, 0, header, sizeof header)) return NULL;
    dirs = le_bytes(header + 0x18, 8);
    dirs_size = le_bytes(header + 0x20, 8);
    table = le_bytes(header + 0x38, 8);
    table_size = le_bytes(header + 0x40, 8);
    files = le_bytes(header + 0x48, 8);
    if (dirs > total || dirs_size > total - dirs) return NULL;
    while ((slash = strchr(name, '/')) != NULL) { /* the next directory of the path, among dir's children */
        want = (size_t)(slash - name);
        if (dir > dirs_size || 0x18 > dirs_size - dir || snx_hac_fs_read_romfs(fs, dirs + dir, entry, 0x18))
            return NULL;
        at = (SnxU32)le_bytes(entry + 8, 4);
        for (visited = 0; at != 0xFFFFFFFFUL && visited < 0x100000; visited++) {
            if (at > dirs_size || 0x18 > dirs_size - at || snx_hac_fs_read_romfs(fs, dirs + at, entry, 0x18))
                return NULL;
            name_size = (SnxU32)le_bytes(entry + 0x14, 4);
            if (name_size == want && name_size <= 0x300 && 0x18 + (SnxU64)name_size <= dirs_size - at &&
                !snx_hac_fs_read_romfs(fs, dirs + at + 0x18, entry + 0x18, name_size) &&
                !memcmp(entry + 0x18, name, want))
                break;
            at = (SnxU32)le_bytes(entry + 4, 4);
        }
        if (at == 0xFFFFFFFFUL || visited == 0x100000) return NULL;
        dir = at;
        name = slash + 1;
    }
    want = strlen(name);
    if (dir > dirs_size || 0x10 > dirs_size - dir || snx_hac_fs_read_romfs(fs, dirs + dir, entry, 0x10))
        return NULL; /* the directory's first file */
    at = (SnxU32)le_bytes(entry + 0xc, 4);
    for (visited = 0; at != 0xFFFFFFFFUL && visited < 0x100000; visited++) {
        if (table > total || table_size > total - table || at > table_size || 0x20 > table_size - at ||
            snx_hac_fs_read_romfs(fs, table + at, entry, 0x20))
            return NULL;
        name_size = (SnxU32)le_bytes(entry + 0x1c, 4);
        if (name_size == want && name_size <= 0x300 && 0x20 + (SnxU64)name_size <= table_size - at &&
            !snx_hac_fs_read_romfs(fs, table + at + 0x20, entry + 0x20, name_size) &&
            !memcmp(entry + 0x20, name, want)) {
            start = le_bytes(entry + 8, 8);
            length = le_bytes(entry + 0x10, 8);
            if (files > total || start > total - files || length > total - files - start || length > 0x1000000UL)
                return NULL;
            data = (unsigned char*)malloc(length ? (size_t)length : 1);
            if (data && snx_hac_fs_read_romfs(fs, files + start, data, (size_t)length)) {
                free(data);
                data = NULL;
            }
            if (data) *size = (size_t)length;
            return data;
        }
        at = (SnxU32)le_bytes(entry + 4, 4);
    }
    return NULL;
}

/* the Control NCA's RomFS, or NULL with why in *why */
static SnxHacFs* control_fs(SnxDump* dump, const char** why) {
    SnxHacFs* fs;
    *why = NULL;
    if (!dump->control.found) {
        *why = "it has no Control NCA";
        return NULL;
    }
    fs = snx_hac_fs_open(dump->hac, dump->control.container, dump->control.entry, -1, 0);
    if (!fs) *why = "its Control NCA cannot be read";
    return fs;
}
unsigned char* snx_dump_nacp(SnxDump* dump, size_t* size, const char** why) {
    SnxHacFs* fs = control_fs(dump, why);
    unsigned char* nacp;
    if (!fs) return NULL;
    nacp = snx_romfs_file(fs, "control.nacp", size);
    snx_hac_fs_free(fs);
    if (!nacp) *why = "its Control NCA holds no control.nacp";
    return nacp;
}
unsigned char* snx_dump_icon(SnxDump* dump, size_t* size, const char** why) {
    /* control.nacp's languages, in its order */
    static const char* const languages[] = {
        "AmericanEnglish", "BritishEnglish",       "Japanese",          "French",
        "German",          "LatinAmericanSpanish", "Spanish",           "Italian",
        "Dutch",           "CanadianFrench",       "Portuguese",        "Russian",
        "Korean",          "TraditionalChinese",   "SimplifiedChinese", "BrazilianPortuguese"};
    SnxHacFs* fs = control_fs(dump, why);
    unsigned char* icon = NULL;
    char name[48];
    size_t i;
    if (!fs) return NULL;
    for (i = 0; !icon && i < sizeof languages / sizeof *languages; ++i) {
        snprintf(name, sizeof name, "icon_%s.dat", languages[i]);
        icon = snx_romfs_file(fs, name, size);
    }
    snx_hac_fs_free(fs);
    if (!icon) *why = "its Control NCA holds no icon_<language>.dat";
    return icon;
}
