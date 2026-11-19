/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx --pack: NCAs packed into an NSP or an XCI of the usual kind (a PFS0, or a gamecard image with HEAD), each
   NCA in it plain (hactool/snx_hac.h's snx_hac_plain). Not a DNSP or DXCI: the container and the NCA magics are the
   ordinary ones, and a reader tells a plain NCA by its header, which needs no header_key to read. The NCAs come from a
   folder (an NSP's or an XCI secure partition's files, as a tool unpacked them) or from a dump, or are fabricated from
   an extracted game's folder (fabricate.h) */
#include "nx/runtime/driver/pack.h"
#include "nx/runtime/driver/container.h"
#include "nx/runtime/driver/fabricate.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include "common/platform/file.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE           0x200u  /* a gamecard's page: the XCI's offsets count them */
#define XCI_ROOT       0xF000u /* where the root HFS0 starts, as on a card's image */
#define TICKET_MAX     0x400u
#define PFS0_ENTRY     0x18u
#define HFS0_ENTRY     0x40u
#define HASHED_PREFIX  0x200u  /* the bytes of an NCA a secure partition's entry hashes */

typedef struct {
    char* name;   /* in the output */
    char* path;   /* the file it is in */
    SnxU64 offset, size;
    unsigned char hash[32]; /* of its first HASHED_PREFIX plain bytes, for an XCI */
    int plain;    /* plain already */
} PackNca;

typedef struct {
    const char *input, *out, *keys, *title_keys, *data, *name;
    int xci, force, plan;
} PackOptions;

typedef struct {
    FILE* file;
    SnxU64 written, total;
    double said_at;
    int said;
} PackWriter;

static void pack_help(void) {
    puts("Usage: scarletnx --pack FOLDER|DUMP [options]\n"
         "  FOLDER              NCAs (.nca), as an NSP's or an XCI's secure partition unpacked; .tik files in it\n"
         "                      give the title keys. A folder without NCAs but with secure/ in it packs that\n"
         "  DUMP                An XCI, NSP, DXCI or DNSP, its NCAs packed again\n"
         "  GAME                An extracted game's folder (exefs/ with main.npdm, romfs/, and control.nacp and\n"
         "                      icon.jpg when it has them), made into fabricated NCAs\n"
         "  --nsp               Pack into an NSP (default)\n"
         "  --xci               Pack into an XCI\n"
         "  --out FILE          The output (default: beside the input, FOLDER.nsp or DUMP.decrypted.nsp)\n"
         "  --force             Replace the output when it is there\n"
         "  --keys FILE         prod.keys (default: the data folder's, then ~/.switch)\n"
         "  --title-keys FILE   title.keys (same discovery order; optional)\n"
         "  --data DIR          The data folder, where prod.keys is looked for first\n"
         "  --name NAME         A GAME without control.nacp: its stub's title (default: the folder's name)\n"
         "  --plan              A GAME: say what it would make, reading main.npdm and the files' sizes alone\n"
         "  --help              Show this help");
    puts("Each NCA goes in plain: its header and sections decrypted, everything else as it was. The output is an\n"
         "ordinary NSP (PFS0) or XCI (HEAD), not a DNSP or DXCI. NCAs plain already need no keys. Tickets are left\n"
         "out, as plain NCAs need none.\n"
         "A GAME's NCAs are fabricated: a Program (the ExeFS, the RomFS rebuilt with its IVFC hashes), a Control and\n"
         "a Meta NCA, plain, unsigned, their content IDs the SHA-256 of their headers. A control.nacp the folder\n"
         "lacks is a stub (its title, 64 MiB saves), and the content meta is made up. Each file is read once.");
}

static int has_suffix(const char* text, const char* suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++)
        if (snx_to_lower((unsigned char)text[a - b + i]) != snx_to_lower((unsigned char)suffix[i])) return 0;
    return 1;
}

static char* text_join(const char* a, const char* b) {
    size_t n = strlen(a), m = strlen(b);
    char* text = (char*)malloc(n + m + 1);
    if (!text) return NULL;
    memcpy(text, a, n);
    memcpy(text + n, b, m + 1);
    return text;
}

static void sha256(const void* data, size_t size, unsigned char out[32]) {
    unsigned int n = 32;
    EVP_Digest(data, size, out, &n, EVP_sha256(), NULL);
}

static int parse_options(int argc, char** argv, PackOptions* options) {
    *options = (PackOptions){0};
    for (int i = 0; i < argc; i++) {
        const char** slot = NULL;
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            pack_help();
            return 2;
        }
        if (!strcmp(argv[i], "--xci")) {
            options->xci = 1;
            continue;
        }
        if (!strcmp(argv[i], "--nsp")) {
            options->xci = 0;
            continue;
        }
        if (!strcmp(argv[i], "--force")) {
            options->force = 1;
            continue;
        }
        if (!strcmp(argv[i], "--plan")) {
            options->plan = 1;
            continue;
        }
        if (argv[i][0] != '-' || !argv[i][1]) {
            if (options->input) return snx_error("--pack takes one folder or dump, and %s is a second", argv[i]);
            options->input = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "--out"))
            slot = &options->out;
        else if (!strcmp(argv[i], "--keys"))
            slot = &options->keys;
        else if (!strcmp(argv[i], "--title-keys"))
            slot = &options->title_keys;
        else if (!strcmp(argv[i], "--data"))
            slot = &options->data;
        else if (!strcmp(argv[i], "--name"))
            slot = &options->name;
        if (!slot) return snx_error("unknown option: %s (scarletnx --pack -h lists them)", argv[i]);
        if (i + 1 == argc || !argv[i + 1][0]) return snx_error("%s requires a value", argv[i]);
        *slot = argv[++i];
    }
    if (!options->input) {
        pack_help();
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ what is packed */

typedef struct {
    PackNca* ncas;
    size_t count, capacity;
} PackList;

static int list_add(PackList* list, const char* name, const char* path, SnxU64 offset, SnxU64 size) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        PackNca* ncas = (PackNca*)realloc(list->ncas, capacity * sizeof *ncas);
        if (!ncas) return snx_error("out of memory listing the NCAs");
        list->ncas = ncas;
        list->capacity = capacity;
    }
    PackNca* nca = &list->ncas[list->count];
    *nca = (PackNca){0};
    nca->name = text_join(name, "");
    nca->path = text_join(path, "");
    nca->offset = offset;
    nca->size = size;
    if (!nca->name || !nca->path) {
        free(nca->name);
        free(nca->path);
        return snx_error("out of memory listing the NCAs");
    }
    list->count++;
    return 0;
}

static void list_free(PackList* list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->ncas[i].name);
        free(list->ncas[i].path);
    }
    free(list->ncas);
    *list = (PackList){0};
}

static int by_name(const void* a, const void* b) {
    return strcmp(((const PackNca*)a)->name, ((const PackNca*)b)->name);
}

/* a ticket file's title key into the keyset: 1 when it gave one */
static int ticket_file(SnxDump* dump, const char* path) {
    unsigned char data[TICKET_MAX];
    FILE* file = snx_fopen(path, "rb");
    if (!file) return 0;
    size_t size = fread(data, 1, sizeof data, file);
    fclose(file);
    int given = snx_ticket_key(dump->hac, data, size) == 1;
    memset(data, 0, sizeof data);
    return given;
}

/* the NCAs of folder, and the title keys of its tickets: 0, else nonzero said */
static int list_folder(PackList* list, SnxDump* dump, const char* folder, unsigned* tickets) {
    char** names;
    size_t count;
    if (snx_list_dir(folder, &names, &count)) return snx_error("cannot list %s", folder);
    int failed = 0;
    for (size_t i = 0; i < count && !failed; i++) {
        char* path = snx_path_join(folder, names[i]);
        if (!path) {
            failed = snx_error("out of memory listing %s", folder);
            break;
        }
        if (snx_is_file(path) && has_suffix(names[i], ".nca"))
            failed = list_add(list, names[i], path, 0, snx_file_size(path));
        else if (snx_is_file(path) && has_suffix(names[i], ".tik"))
            *tickets += (unsigned)ticket_file(dump, path);
        free(path);
    }
    snx_free_strings(names, count);
    return failed;
}

/* the NCAs of a dump, and the title keys of its tickets: 0, else nonzero said */
static int list_dump(PackList* list, SnxDump* dump, const char* path, unsigned* tickets) {
    int xci;
    if (snx_container_kind(path, &xci)) return 1;
    int container = snx_hac_open(dump->hac, path, xci);
    if (container < 0) return snx_hac_error(xci ? "cannot read the XCI's secure partition" : "cannot read the NSP");
    const SnxHacEntry* entries;
    size_t count = snx_hac_entries(dump->hac, container, &entries);
    for (size_t i = 0; i < count; i++) {
        if (has_suffix(entries[i].name, ".nca")) {
            if (list_add(list, entries[i].name, path, entries[i].offset, entries[i].size)) return 1;
        } else if (has_suffix(entries[i].name, ".tik")) {
            unsigned char data[TICKET_MAX];
            size_t size = entries[i].size < sizeof data ? (size_t)entries[i].size : sizeof data;
            if (!snx_hac_read(dump->hac, container, i, data, size))
                *tickets += snx_ticket_key(dump->hac, data, size) == 1;
            memset(data, 0, sizeof data);
        }
    }
    return 0;
}

/* each NCA's plain header read, what is said about it, and its hash for an XCI: 0, else nonzero said */
static int read_headers(PackList* list, SnxDump* dump) {
    unsigned char header[0xC00];
    for (size_t i = 0; i < list->count; i++) {
        PackNca* nca = &list->ncas[i];
        FILE* file = snx_fopen(nca->path, "rb");
        if (!file) return snx_error("cannot open %s", nca->path);
        int rc = snx_hac_plain(dump->hac, file, nca->offset, nca->size, header, NULL, NULL, &nca->plain);
        fclose(file);
        if (rc) {
            char what[512];
            snprintf(what, sizeof what, "%s cannot be made plain", nca->name);
            return snx_hac_error(what);
        }
        sha256(header, HASHED_PREFIX, nca->hash);
        if (nca->size % PAGE)
            printf("  %s is %llu bytes, not whole pages of 0x200: an NCA cut short?\n", nca->name,
                   (unsigned long long)nca->size);
        if (!has_suffix(nca->name, ".cnmt.nca") &&
            (strlen(nca->name) != 36 || strspn(nca->name, "0123456789abcdefABCDEF") != 32))
            printf("  %s is not named by its content ID (32 hex digits, .nca): a reader that finds the NCAs through "
                   "the content meta will not find it\n",
                   nca->name);
    }
    memset(header, 0, sizeof header);
    return 0;
}

/* ------------------------------------------------------------------ the output */

static void progress_say(PackWriter* w, int last) {
    SnxU64 done = w->written > w->total ? w->total : w->written;
    printf("  packed %lu of %lu MB (%d%%)%s", (unsigned long)(done >> 20), (unsigned long)(w->total >> 20),
           w->total ? (int)(done * 100 / w->total) : 100, last ? "\n" : "\r");
    fflush(stdout);
    w->said = 1;
}

static int sink(const void* bytes, size_t size, void* data) {
    PackWriter* w = (PackWriter*)data;
    if (size && fwrite(bytes, 1, size, w->file) != size) return -1;
    w->written += size;
    double now = snx_seconds();
    if (now - w->said_at >= 1.0) {
        w->said_at = now;
        progress_say(w, 0);
    }
    return 0;
}

/* count bytes of value */
static int fill(PackWriter* w, int value, SnxU64 count) {
    unsigned char block[0x1000];
    memset(block, value, sizeof block);
    while (count) {
        size_t n = count < sizeof block ? (size_t)count : sizeof block;
        if (sink(block, n, w)) return -1;
        count -= n;
    }
    return 0;
}

/* the NCAs, each plain, one after the other: 0, else nonzero said */
static int write_ncas(PackWriter* w, const PackList* list, SnxDump* dump) {
    for (size_t i = 0; i < list->count; i++) {
        const PackNca* nca = &list->ncas[i];
        FILE* file = snx_fopen(nca->path, "rb");
        if (!file) return snx_error("cannot open %s", nca->path);
        int rc = snx_hac_plain(dump->hac, file, nca->offset, nca->size, NULL, sink, w, NULL);
        fclose(file);
        if (rc) {
            char what[512];
            snprintf(what, sizeof what, "%s cannot be written plain", nca->name);
            return snx_hac_error(what);
        }
    }
    return 0;
}

/* a PFS0 or HFS0 header of list's files (none for an empty partition), padded to a multiple of align, malloc'd with
   its size in *size (snx_partition_header). An HFS0 entry hashes its NCA's first HASHED_PREFIX bytes */
static unsigned char* fs_header(const PackList* list, int hfs0, SnxU64 align, size_t* size) {
    size_t count = list ? list->count : 0;
    const char** names = (const char**)calloc(count ? count : 1, sizeof *names);
    SnxU64* sizes = (SnxU64*)calloc(count ? count : 1, sizeof *sizes);
    uint32_t* hashed = (uint32_t*)calloc(count ? count : 1, sizeof *hashed);
    unsigned char(*hashes)[32] = (unsigned char(*)[32])calloc(count ? count : 1, 32);
    unsigned char* h = NULL;
    if (names && sizes && hashed && hashes) {
        for (size_t i = 0; i < count; i++) {
            names[i] = list->ncas[i].name;
            sizes[i] = list->ncas[i].size;
            hashed[i] = HASHED_PREFIX;
            memcpy(hashes[i], list->ncas[i].hash, 32);
        }
        h = snx_partition_header(names, sizes, hashed, (const unsigned char(*)[32])hashes, count, hfs0, align, size);
    }
    free(names);
    free(sizes);
    free(hashed);
    free(hashes);
    return h;
}

/* the gamecard's size, by what it holds */
static int cart_type(SnxU64 bytes) {
    static const struct {
        SnxU64 gb;
        int type;
    } sizes[] = {{1, 0xFA}, {2, 0xF8}, {4, 0xF0}, {8, 0xE0}, {16, 0xE1}};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++)
        if (bytes <= sizes[i].gb << 30) return sizes[i].type;
    return 0xE2; /* 32 GB */
}

/* the bytes of the container before its NCAs, malloc'd, its size in *size: an NSP's PFS0 header, or an XCI's header,
   its fill, its root partition and the headers of its partitions; and in *tail the zeros after the NCAs that end the
   XCI's last page. say: what is left out of the XCI's header said. NULL, said why */
static unsigned char* container_prefix(const PackList* list, int xci, SnxDump* dump, int say, size_t* size,
                                       SnxU64* tail) {
    *tail = 0;
    if (!xci) {
        unsigned char* header = fs_header(list, 0, 0x20, size);
        if (!header) snx_error("out of memory writing the NSP's header");
        return header;
    }
    size_t empty_size, secure_size, root_size;
    unsigned char* empty = fs_header(NULL, 1, PAGE, &empty_size);
    unsigned char* secure = fs_header(list, 1, PAGE, &secure_size);
    if (!empty || !secure) {
        free(empty);
        free(secure);
        snx_error("out of memory writing the XCI's partitions");
        return NULL;
    }
    /* the root partition: update and normal empty, then the secure one, each entry hashing its partition's header */
    PackNca parts[3] = {{0}};
    PackList root = {parts, 3, 3};
    const char* part_names[3] = {"update", "normal", "secure"};
    for (int i = 0; i < 3; i++) {
        parts[i].name = (char*)part_names[i];
        parts[i].size = i < 2 ? empty_size : secure_size;
        sha256(i < 2 ? empty : secure, (size_t)parts[i].size, parts[i].hash);
    }
    SnxU64 data = 0;
    for (size_t i = 0; i < list->count; i++) data += list->ncas[i].size;
    parts[2].size += data;
    unsigned char* top = fs_header(&root, 1, PAGE, &root_size);
    SnxU64 secure_at = XCI_ROOT + root_size + 2 * empty_size;
    unsigned char* out = top ? (unsigned char*)malloc((size_t)(secure_at + secure_size)) : NULL;
    if (!out) {
        free(top);
        free(empty);
        free(secure);
        snx_error("out of memory writing the XCI's root partition");
        return NULL;
    }
    for (int i = 0; i < 3; i++) /* the root's entries hash each partition's header alone */
        snx_st_le32(top + 0x10 + (size_t)i * HFS0_ENTRY + 20, (SnxU32)(i < 2 ? empty_size : secure_size));
    SnxU64 end = secure_at + secure_size + data;
    SnxU64 pages = snx_div_up(end, PAGE);

    unsigned char* h = out;
    memset(h, 0, PAGE);
    memcpy(h + 0x100, "HEAD", 4);
    snx_st_le32(h + 0x104, (SnxU32)(secure_at / PAGE)); /* the secure area's first page */
    snx_st_le32(h + 0x108, 0xFFFFFFFFu);                /* no backup area */
    h[0x10D] = (unsigned char)cart_type(pages * PAGE);
    snx_st_le64(h + 0x118, pages - 1); /* the last page of valid data */
    snx_st_le64(h + 0x130, XCI_ROOT);
    snx_st_le64(h + 0x138, root_size);
    sha256(top, root_size, h + 0x140);
    unsigned char iv[16];
    memcpy(iv, h + 0x140 + 16, sizeof iv);                  /* a fixed IV, from what the card holds */
    for (int i = 0; i < 16; i++) h[0x120 + i] = iv[15 - i]; /* stored reversed */
    snx_st_le32(h + 0x180, 1);                              /* SelSec */
    snx_st_le32(h + 0x184, 2);                              /* SelT1Key */
    snx_st_le32(h + 0x188, 0);                              /* SelKey */
    snx_st_le32(h + 0x18C, (SnxU32)(pages - 1));            /* LimArea */
    {
        /* the gamecard info: a retail card's firmware at 25 MHz, the global region. Encrypted under xci_header_key
           when prod.keys has it, else left zero: a reader without the key checks the root's hash alone */
        unsigned char info[0x70] = {0};
        snx_st_le64(info, 2);
        snx_st_le32(info + 8, 0x00A10011u);
        snx_st_le32(info + 0xC, 0x1388u);
        snx_st_le64(info + 0x30, 0x0100000000000816ull);
        if (snx_hac_xci_info(dump->hac, iv, info, h + 0x190) && say)
            puts("  prod.keys has no xci_header_key: the XCI's gamecard info is left zero");
    }
    memset(out + PAGE, 0xFF, XCI_ROOT - PAGE);
    memcpy(out + XCI_ROOT, top, root_size);
    memcpy(out + XCI_ROOT + root_size, empty, empty_size);
    memcpy(out + XCI_ROOT + root_size + empty_size, empty, empty_size);
    memcpy(out + secure_at, secure, secure_size);
    free(top);
    free(empty);
    free(secure);
    *size = (size_t)(secure_at + secure_size);
    *tail = end % PAGE ? PAGE - end % PAGE : 0;
    return out;
}

/* the container of list's NCAs: their plain forms, or fab's fabricated NCAs (list then names them, as the zeros they
   are named by until written, then their names). A fabricated pack writes its container's bytes before the NCAs
   again once the NCAs are named, and fab's patches over what it wrote, after the last byte is written: 0, else
   nonzero, said why */
static int write_container(PackWriter* w, PackList* list, SnxDump* dump, int xci, SnxFab* fab) {
    size_t prefix_size;
    SnxU64 tail;
    unsigned char* prefix = container_prefix(list, xci, dump, 1, &prefix_size, &tail);
    if (!prefix) return 1;
    int failed = sink(prefix, prefix_size, w) ? snx_error("cannot write the container's header") : 0;
    free(prefix);
    if (!failed) failed = fab ? snx_fab_write(fab, sink, w) : write_ncas(w, list, dump);
    if (!failed && tail && fill(w, 0, tail)) failed = snx_error("cannot write the XCI's last page");
    if (failed || !fab) return failed;
    for (size_t i = 0; i < list->count; i++) {
        char* name = text_join(snx_fab_name(fab, i), "");
        if (!name) return snx_error("out of memory");
        free(list->ncas[i].name);
        list->ncas[i].name = name;
    }
    size_t again_size;
    SnxU64 again_tail;
    prefix = container_prefix(list, xci, dump, 0, &again_size, &again_tail);
    if (!prefix) return 1;
    if (again_size != prefix_size) failed = snx_error("the container's header came out a different size");
    if (!failed && fflush(w->file)) failed = snx_error("cannot write the container");
    if (!failed && plat_pwrite(w->file, prefix, prefix_size, 0) != (int64_t)prefix_size)
        failed = snx_error("cannot write the container's header again");
    free(prefix);
    const SnxFabPatch* patches;
    size_t count = snx_fab_patches(fab, &patches);
    for (size_t i = 0; i < count && !failed; i++)
        if (plat_pwrite(w->file, patches[i].bytes, patches[i].size, prefix_size + patches[i].at) !=
            (int64_t)patches[i].size)
            failed = snx_error("cannot write the NCAs' hashes and headers");
    return failed;
}

/* the output's default path, beside the input: FOLDER.nsp, or DUMP.decrypted.nsp */
static char* default_out(const char* input, int folder, int xci) {
    size_t n = strlen(input);
    while (n > 1 && snx_is_any_separator((unsigned char)input[n - 1])) n--;
    char* stem = (char*)malloc(n + 1);
    if (!stem) return NULL;
    memcpy(stem, input, n);
    stem[n] = 0;
    if (!folder) {
        char* dot = strrchr(stem, '.');
        if (dot && dot > snx_path_leaf(stem)) *dot = 0;
    }
    char* out = text_join(stem, folder ? (xci ? ".xci" : ".nsp") : (xci ? ".decrypted.xci" : ".decrypted.nsp"));
    free(stem);
    return out;
}

int snx_pack(int argc, char** argv, const SnxPaths* paths) {
    PackOptions options;
    int parsed = parse_options(argc, argv, &options);
    if (parsed) return parsed == 2 ? 0 : 1;
    if (!options.keys && paths) options.keys = paths->keys;
    if (!options.title_keys && paths) options.title_keys = paths->title_keys;
    const char* repo = options.data ? options.data : paths ? paths->repo : NULL;
    int folder = snx_is_dir(options.input);
    if (!folder && !snx_is_file(options.input)) return snx_error("%s is neither a folder nor a file", options.input);
    char* out = options.out ? text_join(options.out, "") : default_out(options.input, folder, options.xci);
    char* part = out ? text_join(out, ".part") : NULL;
    SnxDump dump;
    PackList list = {0};
    PackWriter w = {0};
    SnxFab* fab = NULL;
    int rc = 1;
    if (snx_dump_keys(&dump, options.keys, options.title_keys, repo, !options.plan)) goto done;
    if (!out || !part) {
        snx_error("out of memory");
        goto done;
    }
    if (snx_path_exists(out) && !options.force && !options.plan) {
        snx_error("%s is there already: give --force to replace it, or --out another file", out);
        goto done;
    }
    unsigned tickets = 0;
    printf("Reading %s\n", options.input);
    if (folder) {
        if (list_folder(&list, &dump, options.input, &tickets)) goto done;
        if (!list.count) { /* an XCI unpacked into its partitions: the secure one */
            char* secure = snx_path_join(options.input, "secure");
            int failed = secure && snx_is_dir(secure) && list_folder(&list, &dump, secure, &tickets);
            free(secure);
            if (failed) goto done;
        }
    } else if (list_dump(&list, &dump, options.input, &tickets))
        goto done;
    if (!list.count && folder && snx_fab_is_game(options.input)) { /* an extracted game: its NCAs fabricated */
        if (!(fab = snx_fab_plan(options.input, options.name, 1))) goto done;
        static const unsigned char zeros[HASHED_PREFIX];
        unsigned char first[32]; /* a fabricated NCA's first page: its signatures, zero */
        sha256(zeros, sizeof zeros, first);
        for (size_t i = 0; i < snx_fab_count(fab); i++) {
            if (list_add(&list, snx_fab_name(fab, i), "", 0, snx_fab_size(fab, i))) goto done;
            memcpy(list.ncas[i].hash, first, 32);
            w.total += snx_fab_size(fab, i);
        }
        char size[32];
        snx_size_text(w.total, size, sizeof size);
        printf("  the %s: about %s\n", options.xci ? "XCI" : "NSP", size);
        if (options.plan) {
            rc = 0;
            goto done;
        }
    } else if (options.plan) {
        snx_error("--plan is for an extracted game's folder (exefs/ with main), and %s is none", options.input);
        goto done;
    } else if (options.name) {
        snx_error("--name is for an extracted game's folder (exefs/ with main), and %s is none", options.input);
        goto done;
    }
    if (!list.count) {
        snx_error("%s has no NCAs (.nca) to pack, and is no extracted game's folder (exefs/ with main)",
                  options.input);
        goto done;
    }
    if (!fab) {
        qsort(list.ncas, list.count, sizeof *list.ncas, by_name);
        if (tickets) printf("  %u title key%s from the tickets\n", tickets, tickets == 1 ? "" : "s");
        if (read_headers(&list, &dump)) goto done;
        unsigned plain = 0;
        for (size_t i = 0; i < list.count; i++) {
            w.total += list.ncas[i].size;
            plain += (unsigned)list.ncas[i].plain;
        }
        printf("  %lu NCA%s, %u plain already, %lu MB\n", (unsigned long)list.count, list.count == 1 ? "" : "s",
               plain, (unsigned long)(w.total >> 20));
    }
    if (snx_same_file(options.input, out)) {
        snx_error("the output %s is the input", out);
        goto done;
    }
    printf("Packing %s\n", out);
    fflush(stdout);
    w.file = snx_fopen(part, "wb");
    if (!w.file) {
        char why[256];
        snx_error("cannot create %s: %s", part, snx_os_error(why, sizeof why));
        goto done;
    }
    w.said_at = snx_seconds();
    int failed = write_container(&w, &list, &dump, options.xci, fab);
    if (w.said) progress_say(&w, 1);
    if (fclose(w.file) && !failed) failed = snx_error("cannot finish writing %s", part);
    w.file = NULL;
    if (!failed && plat_replace_file(part, out)) failed = snx_error("cannot rename %s to %s", part, out);
    if (failed) {
        remove(part);
        goto done;
    }
    char size[32];
    snx_size_text(snx_file_size(out), size, sizeof size);
    printf("Packed %s: %s, an %s of %s NCAs\n", out, size, options.xci ? "XCI" : "NSP",
           fab ? "fabricated plain" : "plain");
    rc = 0;
done:
    snx_fab_free(fab);
    list_free(&list);
    snx_dump_close(&dump);
    free(part);
    free(out);
    return rc;
}
