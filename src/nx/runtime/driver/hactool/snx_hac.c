/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_hac.h over hactool's own contexts, as its main.c drives them for -txci, -tpfs0, -i and --basenca, but with every
   NCA read where it lies in its container (nca_ctx_t.base_offset) instead of from a copy unpacked beside it */
#include "snx_hac.h"
#include <stdarg.h>
#include <string.h>
#include "extkeys.h"
#include "filepath.h"
#include "hfs0.h"
#include "nca.h"
#include "pfs0.h"
#include "pki.h"
#include "storage.h"
#include "xci.h"

/* the C library's own here, under the names snx_io.h (included ahead of this file too) gives hactool */
#undef printf
#undef fprintf
#undef fwrite
#undef exit

#define SNX_HAC_CONTAINERS 2

typedef struct {
    FILE *file;
    xci_ctx_t *xci;   /* an XCI's contexts, NULL for an NSP */
    pfs0_ctx_t *pfs0; /* an NSP's */
    SnxHacEntry *entries;
    size_t count;
    const unsigned char *map; /* the file mapped (snx_hac_map), map_size bytes, or NULL */
    uint64_t map_size;
} Container;

struct SnxHac {
    hactool_ctx_t tool;
    Container containers[SNX_HAC_CONTAINERS];
    int open;
};

/* ------------------------------------------------------------------ hactool's output, exits and writes */

static struct {
    char message[256]; /* the last line on stderr */
    void (*tick)(uint64_t, void *);
    void *tick_data;
    uint64_t written;
    void (*cleanup)(void *);
    void *cleanup_data;
} io;

static void io_say(FILE *file, const char *format, va_list ap) {
    char line[sizeof io.message];
    size_t n;
    if (file != stderr) return; /* stdout is hactool's -i report, keys among it */
    vsnprintf(line, sizeof line, format, ap);
    n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n) memcpy(io.message, line, n + 1);
}

int snx_hac_printf(const char *format, ...) {
    (void)format;
    return 0;
}

int snx_hac_fprintf(FILE *file, const char *format, ...) {
    va_list ap;
    int n = 0;
    va_start(ap, format);
    if (file == stdout || file == stderr)
        io_say(file, format, ap);
    else
        n = vfprintf(file, format, ap);
    va_end(ap);
    return n;
}

size_t snx_hac_fwrite(const void *data, size_t size, size_t count, FILE *file) {
    size_t n;
    if (file == stdout || file == stderr) return count;
    n = fwrite(data, size, count, file);
    io.written += (uint64_t)n * size;
    if (io.tick) io.tick(io.written, io.tick_data);
    return n;
}

_Noreturn void snx_hac_exit(int status) {
    void (*cleanup)(void *) = io.cleanup;
    io.cleanup = NULL; /* once, should the cleanup itself end in here */
    if (cleanup) cleanup(io.cleanup_data);
    exit(status ? status : EXIT_FAILURE);
}

void snx_hac_progress(void (*tick)(uint64_t written, void *data), void *data) {
    io.tick = tick;
    io.tick_data = data;
    io.written = 0;
}

void snx_hac_on_exit(void (*cleanup)(void *data), void *data) {
    io.cleanup = cleanup;
    io.cleanup_data = data;
}

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

const char *snx_hac_message(void) {
    static char said[sizeof io.message];
    size_t i = 0, run, out = 0;
    while (io.message[i]) {
        for (run = 0; is_hex(io.message[i + run]); run++) {}
        if (run >= 16) { /* "..." is no longer than what it stands for */
            memcpy(said + out, "...", 3);
            out += 3;
        } else {
            if (!run) run = 1;
            memcpy(said + out, io.message + i, run);
            out += run;
        }
        i += run;
    }
    said[out] = 0;
    return said;
}

/* ------------------------------------------------------------------ keys */

SnxHac *snx_hac_new(void) {
    SnxHac *hac = calloc(1, sizeof *hac);
    if (!hac) return NULL;
    pki_initialize_keyset(&hac->tool.settings.keyset, KEYSET_RETAIL);
    hac->tool.settings.skip_key_warnings = 1;
    return hac;
}

int snx_hac_set_key(SnxHac *hac, const char *name, const char *value) {
    return extkeys_set_key(&hac->tool.settings, name, value);
}

void snx_hac_derive_keys(SnxHac *hac) {
    hactool_settings_t *settings = &hac->tool.settings;
    if (settings->has_sdseed)
        for (unsigned int key = 0; key < 2; key++)
            for (unsigned int i = 0; i < 0x20; i++) settings->keyset.sd_card_key_sources[key][i] ^= settings->sdseed[i & 0xF];
    pki_derive_keys(&settings->keyset);
}

void snx_hac_add_title_key(SnxHac *hac, const unsigned char rights_id[16], const unsigned char key[16]) {
    if (!settings_has_titlekey(&hac->tool.settings, rights_id))
        settings_add_titlekey(&hac->tool.settings, rights_id, key);
}

/* ------------------------------------------------------------------ containers */

static void container_close(Container *c) {
    if (c->xci) {
        hfs0_ctx_t *parts[5] = {&c->xci->partition_ctx, &c->xci->normal_ctx, &c->xci->update_ctx, &c->xci->secure_ctx,
                                &c->xci->logo_ctx};
        for (unsigned int i = 0; i < 5; i++) free(parts[i]->header);
        free(c->xci);
    }
    if (c->pfs0) {
        free(c->pfs0->header);
        free(c->pfs0->npdm);
        free(c->pfs0);
    }
    free(c->entries);
    if (c->file) fclose(c->file);
    memset(c, 0, sizeof *c);
}

void snx_hac_free(SnxHac *hac) {
    if (!hac) return;
    for (int i = 0; i < hac->open; i++) container_close(&hac->containers[i]);
    free(hac->tool.settings.known_titlekeys.titlekeys);
    memset(hac, 0, sizeof *hac); /* the keys */
    free(hac);
}

int snx_hac_open(SnxHac *hac, const char *path, int xci) {
    Container *c;
    hactool_ctx_t parse;
    if (hac->open == SNX_HAC_CONTAINERS) return -1;
    c = &hac->containers[hac->open];
    filepath_t name;
    filepath_init(&name);
    filepath_set(&name, path);
    if (name.valid != VALIDITY_VALID || !(c->file = os_fopen(name.os_path, OS_MODE_READ))) return -1;
    parse = hac->tool;
    parse.action = 0; /* read the tables, print and save nothing */
    parse.file = c->file;
    if (xci) {
        hfs0_header_t *h;
        uint64_t data;
        if (!(c->xci = calloc(1, sizeof *c->xci))) goto fail;
        c->xci->file = c->file;
        c->xci->tool_ctx = &parse;
        xci_process(c->xci);
        c->xci->tool_ctx = NULL;
        h = c->xci->secure_ctx.header;
        if (!h) goto fail; /* no secure partition: nothing of a game in it */
        data = c->xci->secure_ctx.offset + hfs0_get_header_size(h);
        if (!(c->entries = calloc(h->num_files ? h->num_files : 1, sizeof *c->entries))) goto fail;
        for (uint32_t i = 0; i < h->num_files; i++) {
            hfs0_file_entry_t *e = hfs0_get_file_entry(h, i);
            c->entries[i].name = hfs0_get_file_name(h, i);
            c->entries[i].offset = data + e->offset;
            c->entries[i].size = e->size;
        }
        c->count = h->num_files;
    } else {
        pfs0_header_t *h;
        uint64_t data;
        if (!(c->pfs0 = calloc(1, sizeof *c->pfs0))) goto fail;
        c->pfs0->file = c->file;
        c->pfs0->tool_ctx = &parse;
        pfs0_process(c->pfs0);
        c->pfs0->tool_ctx = NULL;
        h = c->pfs0->header;
        data = pfs0_get_header_size(h);
        if (!(c->entries = calloc(h->num_files ? h->num_files : 1, sizeof *c->entries))) goto fail;
        for (uint32_t i = 0; i < h->num_files; i++) {
            pfs0_file_entry_t *e = pfs0_get_file_entry(h, i);
            c->entries[i].name = pfs0_get_file_name(h, i);
            c->entries[i].offset = data + e->offset;
            c->entries[i].size = e->size;
        }
        c->count = h->num_files;
    }
    return hac->open++;
fail:
    container_close(c);
    return -1;
}

size_t snx_hac_entries(SnxHac *hac, int container, const SnxHacEntry **entries) {
    *entries = hac->containers[container].entries;
    return hac->containers[container].count;
}

int snx_hac_read(SnxHac *hac, int container, size_t entry, void *buffer, size_t size) {
    Container *c = &hac->containers[container];
    if (size > c->entries[entry].size || fseeko64(c->file, c->entries[entry].offset, SEEK_SET) ||
        fread(buffer, 1, size, c->file) != size)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ NCAs */

static void nca_open(SnxHac *hac, hactool_ctx_t *tool, nca_ctx_t *nca, int container, size_t entry) {
    Container *c = &hac->containers[container];
    nca_init(nca);
    nca->tool_ctx = tool;
    nca->file = c->file;
    nca->base_offset = c->entries[entry].offset;
    nca->file_size = (size_t)c->entries[entry].size;
    nca->map = c->map;
    nca->map_size = c->map_size;
}

int snx_hac_newest_generation(SnxHac *hac) {
    static const unsigned char zero[0x10];
    int generation;
    for (generation = 0x1F; generation >= 0; generation--)
        if (memcmp(hac->tool.settings.keyset.key_area_keys[generation][0], zero, 0x10)) return generation;
    return -1;
}

/* a Patch RomFS in the NCA whose header is decrypted, as nca_process tells one: BKTR encryption, or a decrypted dump's
   kept indirect tables. An update's patches its base's RomFS, which it is read over */
static int patches_romfs(const nca_ctx_t *nca) {
    for (unsigned int i = 0; i < 4; i++) {
        const nca_fs_header_t *fs = &nca->header.fs_headers[i];
        if (!nca->header.section_entries[i].media_start_offset || fs->partition_type != PARTITION_ROMFS ||
            fs->fs_type != FS_TYPE_ROMFS)
            continue;
        if (fs->crypt_type == CRYPT_BKTR ||
            (nca->is_decrypted && fs->bktr_superblock.relocation_header.magic == MAGIC_BKTR &&
             fs->bktr_superblock.subsection_header.magic == MAGIC_BKTR))
            return 1;
    }
    return 0;
}

/* 1 when the keyset has the key of generation (below 0x20) that opens nca's title key (rights) or its key area */
static int has_key(SnxHac *hac, const nca_ctx_t *nca, int rights, int generation) {
    static const unsigned char zero[0x10];
    const nca_keyset_t *keys = &hac->tool.settings.keyset;
    if (rights) return memcmp(keys->titlekeks[generation], zero, 0x10) != 0;
    return nca->header.kaek_ind < 3 && memcmp(keys->key_area_keys[generation][nca->header.kaek_ind], zero, 0x10) != 0;
}

int snx_hac_nca(SnxHac *hac, int container, size_t entry, SnxHacNca *out) {
    nca_ctx_t nca;
    memset(out, 0, sizeof *out);
    nca_open(hac, &hac->tool, &nca, container, entry);
    if (!nca_decrypt_header(&nca)) return -1;
    out->type = nca.header.content_type == NCACONTENTTYPE_PROGRAM   ? SNX_HAC_PROGRAM
                : nca.header.content_type == NCACONTENTTYPE_CONTROL ? SNX_HAC_CONTROL
                : nca.header.content_type == NCACONTENTTYPE_DATA || nca.header.content_type == NCACONTENTTPYE_PUBLICDATA
                    ? SNX_HAC_DATA
                    : SNX_HAC_OTHER;
    out->title_id = nca.header.title_id;
    out->content = nca.header.content_type;
    out->decrypted = nca.is_decrypted;
    out->sdk = nca.header.sdk_version;
    /* as nca_process sorts the generation out: 0 and 1 are both master key 0 */
    out->generation = nca.header.crypto_type2 > nca.header.crypto_type ? nca.header.crypto_type2 : nca.header.crypto_type;
    if (out->generation) out->generation--;
    {
        static const unsigned char zero[0x10];
        int rights = memcmp(nca.header.rights_id, zero, 0x10) != 0;
        out->title_key = !rights ? 0 : settings_has_titlekey(&hac->tool.settings, nca.header.rights_id) ? 1 : -1;
        out->keys = out->generation < 0x20 && has_key(hac, &nca, rights, out->generation); /* past 0x1F: never given */
        /* how far the keys file reaches, which a generation it lacks is said against */
        for (out->newest = 0x1F; out->newest >= 0 && !has_key(hac, &nca, rights, out->newest); out->newest--) continue;
    }
    /* a Patch RomFS, as nca_process tells one: BKTR encryption, or a decrypted dump's kept indirect tables */
    out->patch = patches_romfs(&nca);
    return 0;
}

static int has_romfs(const nca_ctx_t *nca) {
    for (unsigned int i = 0; i < 4; i++)
        if (nca->section_contexts[i].is_present && nca->section_contexts[i].type == ROMFS) return 1;
    return 0;
}

/* the NCA at entry processed as tool says, an update's over the base NCA (base_container -1 for none), which base_tool
   and base keep: 0, or -1. Either way the caller frees the section contexts of nca, then of base */
static int program_process(SnxHac *hac, hactool_ctx_t *tool, nca_ctx_t *nca, hactool_ctx_t *base_tool, nca_ctx_t *base,
                           int container, size_t entry, int base_container, size_t base_entry) {
    int alone = 0; /* a base with no RomFS: the update read without it */
    nca_init(nca);
    nca_init(base);
    if (base_container >= 0) { /* --basenca: the base's RomFS under the update's */
        base_tool->settings.keyset = hac->tool.settings.keyset;
        base_tool->settings.known_titlekeys = hac->tool.settings.known_titlekeys;
        nca_open(hac, base_tool, base, base_container, base_entry);
        base->is_cli_target = false;
        if (!nca_decrypt_header(base)) return -1;
        tool->base_file = base->file;
        tool->base_file_type = BASEFILE_NCA;
        tool->base_nca_ctx = base;
        nca_process(base);
        if (!has_romfs(base)) { /* a game all in its ExeFS: an update's RomFS, unless a patch, needs none under it */
            tool->base_file = NULL;
            tool->base_file_type = BASEFILE_FAKE;
            tool->base_nca_ctx = NULL;
            alone = 1;
        }
    }
    nca_open(hac, tool, nca, container, entry);
    nca->is_cli_target = true;
    if (!nca_decrypt_header(nca)) return -1;
    if (alone && patches_romfs(nca)) {
        snx_hac_fprintf(stderr, "The update patches a RomFS its game's Program NCA does not have\n");
        return -1;
    }
    nca_process(nca);
    return 0;
}

static void program_free(nca_ctx_t *nca, nca_ctx_t *base) {
    nca_free_section_contexts(nca); /* the update's first: its storages read through the base's */
    nca_free_section_contexts(base);
}

int snx_hac_extract(SnxHac *hac, int container, size_t entry, int base_container, size_t base_entry,
                    const char *exefs_dir, const char *romfs_dir) {
    hactool_ctx_t *tool, *base_tool;
    nca_ctx_t *nca, *base;
    int rc = -1;
    tool = calloc(1, sizeof *tool);
    base_tool = calloc(1, sizeof *base_tool);
    nca = calloc(1, sizeof *nca);
    base = calloc(1, sizeof *base);
    if (tool && base_tool && nca && base) {
        tool->settings = hac->tool.settings;
        tool->action = ACTION_EXTRACT;
        if (exefs_dir) {
            tool->settings.exefs_dir_path.enabled = 1;
            filepath_set(&tool->settings.exefs_dir_path.path, exefs_dir);
        }
        if (romfs_dir) {
            tool->settings.romfs_dir_path.enabled = 1;
            filepath_set(&tool->settings.romfs_dir_path.path, romfs_dir);
        }
        rc = program_process(hac, tool, nca, base_tool, base, container, entry, base_container, base_entry);
        program_free(nca, base);
    }
    free(base);
    free(nca);
    free(base_tool);
    free(tool);
    return rc;
}

/* ------------------------------------------------------------------ file systems read in place */

struct SnxHacFs {
    hactool_ctx_t tool, base_tool;
    nca_ctx_t nca, base;
    nca_section_ctx_t *exefs; /* NULL without an ExeFS */
    SnxHacEntry *files;       /* its files, offsets in the section */
    size_t count;
    storage_t *romfs; /* the RomFS image's storage, NULL without one */
    uint64_t romfs_offset, romfs_size;
};

int snx_hac_map(SnxHac *hac, int container, const void *data, uint64_t size) {
    Container *c = &hac->containers[container];
    for (size_t i = 0; i < c->count; i++) /* an entry past the mapping's end: not the file opened */
        if (c->entries[i].offset > size || c->entries[i].size > size - c->entries[i].offset) return -1;
    c->map = data;
    c->map_size = size;
    return 0;
}

SnxHacFs *snx_hac_fs_open(SnxHac *hac, int container, size_t entry, int base_container, size_t base_entry) {
    SnxHacFs *fs = calloc(1, sizeof *fs);
    if (!fs) return NULL;
    fs->tool.settings = hac->tool.settings;
    fs->tool.action = 0; /* the tables read, nothing printed or saved */
    if (program_process(hac, &fs->tool, &fs->nca, &fs->base_tool, &fs->base, container, entry, base_container,
                        base_entry))
        goto fail;
    for (unsigned int i = 0; i < 4; i++) {
        nca_section_ctx_t *section = &fs->nca.section_contexts[i];
        if (!section->is_present || section->storage == NULL) continue;
        if (section->type == PFS0 && section->pfs0_ctx.is_exefs && !fs->exefs) {
            pfs0_header_t *h = section->pfs0_ctx.header;
            uint64_t data = section->pfs0_ctx.superblock->pfs0_offset + pfs0_get_header_size(h);
            fs->exefs = section;
            if (!(fs->files = calloc(h->num_files ? h->num_files : 1, sizeof *fs->files))) goto fail;
            for (uint32_t j = 0; j < h->num_files; j++) {
                pfs0_file_entry_t *e = pfs0_get_file_entry(h, j);
                fs->files[j].name = pfs0_get_file_name(h, j);
                fs->files[j].offset = data + e->offset;
                fs->files[j].size = e->size;
            }
            fs->count = h->num_files;
        } else if ((section->type == ROMFS || section->type == BKTR) && !fs->romfs) {
            ivfc_level_ctx_t *data = section->type == ROMFS ? &section->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1]
                                                            : &section->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1];
            if (section->type == BKTR && section->bktr_ctx.indirect == NULL) continue; /* an update without its base */
            fs->romfs = section->storage;
            fs->romfs_offset = data->data_offset;
            /* the decompressed image when the hashes cover compressed blocks */
            fs->romfs_size = section->compressed ? section->compressed->size : data->data_size;
            if (fs->romfs_offset > fs->romfs->size || fs->romfs_size > fs->romfs->size - fs->romfs_offset) goto fail;
        }
    }
    return fs;
fail:
    snx_hac_fs_free(fs);
    return NULL;
}

void snx_hac_fs_free(SnxHacFs *fs) {
    if (!fs) return;
    program_free(&fs->nca, &fs->base);
    free(fs->files);
    memset(fs, 0, sizeof *fs); /* the keys */
    free(fs);
}

int snx_hac_cnmt(SnxHac *hac, int container, size_t entry, unsigned char **data, size_t *size) {
    hactool_ctx_t *tool = calloc(1, sizeof *tool), *base_tool = calloc(1, sizeof *base_tool);
    nca_ctx_t *nca = calloc(1, sizeof *nca), *base = calloc(1, sizeof *base);
    int rc = -1;
    *data = NULL;
    *size = 0;
    if (tool && base_tool && nca && base) {
        tool->settings = hac->tool.settings;
        tool->action = 0; /* the tables read, nothing printed or saved */
        if (!program_process(hac, tool, nca, base_tool, base, container, entry, -1, 0))
            for (unsigned int i = 0; rc && i < 4; i++) {
                nca_section_ctx_t *section = &nca->section_contexts[i];
                pfs0_header_t *h;
                if (!section->is_present || section->type != PFS0 || !section->storage || !section->pfs0_ctx.header)
                    continue;
                h = section->pfs0_ctx.header;
                for (uint32_t j = 0; rc && j < h->num_files; j++) {
                    pfs0_file_entry_t *e = pfs0_get_file_entry(h, j);
                    const char *name = pfs0_get_file_name(h, j);
                    size_t n = strlen(name);
                    uint64_t at = section->pfs0_ctx.superblock->pfs0_offset + pfs0_get_header_size(h) + e->offset;
                    if (n < 5 || strcmp(name + n - 5, ".cnmt") || !e->size || e->size > 0x100000) continue;
                    *data = malloc((size_t)e->size);
                    if (*data && storage_read(section->storage, at, *data, (size_t)e->size)) {
                        *size = (size_t)e->size;
                        rc = 0;
                    } else {
                        free(*data);
                        *data = NULL;
                    }
                }
            }
        program_free(nca, base);
    }
    if (tool) memset(tool, 0, sizeof *tool); /* the keys */
    if (base_tool) memset(base_tool, 0, sizeof *base_tool);
    free(tool);
    free(base_tool);
    free(nca);
    free(base);
    return rc;
}

size_t snx_hac_fs_files(SnxHacFs *fs, const SnxHacEntry **files) {
    *files = fs->files;
    return fs->count;
}

int snx_hac_fs_read(SnxHacFs *fs, size_t file, uint64_t offset, void *buffer, size_t size) {
    const SnxHacEntry *e = &fs->files[file];
    if (offset > e->size || size > e->size - offset) return -1;
    return storage_read(fs->exefs->storage, e->offset + offset, buffer, size) ? 0 : -1;
}

uint64_t snx_hac_fs_romfs_size(const SnxHacFs *fs) { return fs->romfs ? fs->romfs_size : 0; }

int snx_hac_fs_read_romfs(SnxHacFs *fs, uint64_t offset, void *buffer, size_t size) {
    if (!fs->romfs || offset > fs->romfs_size || size > fs->romfs_size - offset) return -1;
    return storage_read(fs->romfs, fs->romfs_offset + offset, buffer, size) ? 0 : -1;
}
