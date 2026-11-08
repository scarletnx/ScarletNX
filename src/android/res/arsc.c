/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/res/arsc.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CHUNK_STRING_POOL = 0x0001,
    CHUNK_TABLE = 0x0002,
    CHUNK_PACKAGE = 0x0200,
    CHUNK_TYPE = 0x0201,
    CHUNK_TYPE_SPEC = 0x0202
};
enum { TYPE_SPARSE = 0x01, TYPE_OFFSET16 = 0x02 };
enum { ENTRY_COMPLEX = 0x0001, ENTRY_COMPACT = 0x0008 };
enum { MAX_TYPES = 256, MAX_DEPTH = 32 };
#define NO_ENTRY 0xffffffffu

/* a type of a package: its spec's flags and its chunks, one for each configuration */
typedef struct {
    const unsigned char* spec; /* the spec's flags, NULL without a spec */
    uint32_t spec_count;
    const unsigned char** chunks;
    uint32_t chunk_count, chunk_capacity;
} Type;
typedef struct {
    uint32_t id;
    char name[128];
    ResStringPool type_names, keys;
    Type types[MAX_TYPES]; /* by type ID, 1 to 255 */
} Package;
typedef struct {
    unsigned char* data; /* the table's bytes, the assets' own copy */
    size_t size;
    ResStringPool strings;
    Package* packages;
    uint32_t package_count;
} Table;
/* a merged bag: a style's entries with its parents' */
typedef struct {
    uint32_t resid, count;
    uint32_t* keys;
    ArscValue* values;
} Bag;
struct ArscAssets {
    Table* tables;
    uint32_t table_count;
    ArscConfig config;
    Bag* bags; /* the bags made for this configuration, by resid in an open-addressed table */
    uint32_t bag_capacity, bag_count;
};
typedef struct {
    uint32_t attr;
    ArscValue value;
} ThemeEntry;
struct ArscTheme {
    ArscAssets* assets;
    ThemeEntry* entries; /* sorted by attr */
    uint32_t count, capacity;
    uint32_t* styles; /* the applied styles, each with its force flag in the top bit */
    uint32_t style_count, style_capacity;
};

/* ---- the tables ---- */

static uint32_t ld32(const unsigned char* p) {
    return snx_ld_le32(p);
}
static uint32_t ld16(const unsigned char* p) {
    return snx_ld_le16(p);
}

static bool add_chunk(Type* t, const unsigned char* chunk) {
    if (t->chunk_count == t->chunk_capacity) {
        uint32_t capacity = t->chunk_capacity ? t->chunk_capacity * 2 : 4;
        const unsigned char** chunks = realloc((void*)t->chunks, capacity * sizeof *chunks);
        if (!chunks) return false;
        t->chunks = chunks;
        t->chunk_capacity = capacity;
    }
    t->chunks[t->chunk_count++] = chunk;
    return true;
}
static bool read_package(Package* p, const unsigned char* chunk, uint32_t size) {
    uint32_t header = ld16(chunk + 2);
    if (header < 284 || header > size) return false;
    p->id = ld32(chunk + 8);
    for (int i = 0; i < 127; ++i) { /* the name's UTF-16 units, ASCII in practice */
        unsigned u = ld16(chunk + 12 + 2 * i);
        p->name[i] = (char)(u < 0x80 ? u : '?');
        if (!u) break;
    }
    uint32_t types = ld32(chunk + 268), keys = ld32(chunk + 276);
    if (types >= size || keys >= size || !res_string_pool(&p->type_names, chunk + types, size - types) ||
        !res_string_pool(&p->keys, chunk + keys, size - keys))
        return false;
    for (uint32_t at = header; at + 8 <= size;) {
        const unsigned char* c = chunk + at;
        uint32_t type = ld16(c), length = ld32(c + 4);
        if (length < 8 || length > size - at) return false;
        if ((type == CHUNK_TYPE || type == CHUNK_TYPE_SPEC) && length >= 20) {
            unsigned id = c[8];
            if (id) {
                Type* t = &p->types[id];
                if (type == CHUNK_TYPE_SPEC) {
                    uint32_t count = ld32(c + 12), hdr = ld16(c + 2);
                    if ((uint64_t)hdr + 4ull * count <= length) {
                        t->spec = c + hdr;
                        t->spec_count = count;
                    }
                } else if (!add_chunk(t, c)) {
                    return false;
                }
            }
        }
        at += length;
    }
    return true;
}

ArscAssets* arsc_create(void) {
    return calloc(1, sizeof(ArscAssets));
}
static void drop_bags(ArscAssets* a) {
    for (uint32_t i = 0; i < a->bag_capacity; ++i) {
        free(a->bags[i].keys);
        free(a->bags[i].values);
    }
    free(a->bags);
    a->bags = NULL;
    a->bag_capacity = a->bag_count = 0;
}
void arsc_destroy(ArscAssets* a) {
    if (!a) return;
    drop_bags(a);
    for (uint32_t i = 0; i < a->table_count; ++i) {
        Table* t = &a->tables[i];
        for (uint32_t p = 0; p < t->package_count; ++p)
            for (int id = 0; id < MAX_TYPES; ++id) free((void*)t->packages[p].types[id].chunks);
        free(t->packages);
        free(t->data);
    }
    free(a->tables);
    free(a);
}
int32_t arsc_add(ArscAssets* a, const unsigned char* data, size_t size, char* error, size_t error_size) {
    if (size < 12 || ld16(data) != CHUNK_TABLE || ld32(data + 4) < 12 || ld32(data + 4) > size) {
        snprintf(error, error_size, "not a resource table");
        return 0;
    }
    /* a package chunk's header alone is 284 bytes */
    if (ld32(data + 8) > ld32(data + 4) / 284) {
        snprintf(error, error_size, "a resource table of %u bytes that names %u packages", (unsigned)ld32(data + 4),
                 (unsigned)ld32(data + 8));
        return 0;
    }
    Table* tables = realloc(a->tables, (a->table_count + 1) * sizeof *tables);
    if (!tables) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    a->tables = tables;
    Table* t = &a->tables[a->table_count];
    *t = (Table){0};
    t->size = ld32(data + 4);
    t->data = malloc(t->size);
    uint32_t packages = ld32(data + 8);
    t->packages = calloc((size_t)packages + 1, sizeof *t->packages);
    if (!t->data || !t->packages) {
        free(t->data);
        free(t->packages);
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    memcpy(t->data, data, t->size);
    bool ok = true;
    for (uint32_t at = ld16(t->data + 2); ok && at + 8 <= t->size;) {
        const unsigned char* c = t->data + at;
        uint32_t type = ld16(c), length = ld32(c + 4);
        if (length < 8 || length > t->size - at) break;
        if (type == CHUNK_STRING_POOL && !t->strings.data)
            ok = res_string_pool(&t->strings, c, length);
        else if (type == CHUNK_PACKAGE && t->package_count < packages)
            ok = read_package(&t->packages[t->package_count++], c, length);
        at += length;
    }
    if (!ok) {
        for (uint32_t p = 0; p < t->package_count; ++p)
            for (int id = 0; id < MAX_TYPES; ++id) free((void*)t->packages[p].types[id].chunks);
        free(t->data);
        free(t->packages);
        snprintf(error, error_size, "a bad resource table");
        return 0;
    }
    ++a->table_count;
    drop_bags(a);
    return (int32_t)a->table_count;
}
void arsc_set_config(ArscAssets* a, const ArscConfig* config) {
    a->config = *config;
    drop_bags(a);
}
const ResStringPool* arsc_strings(const ArscAssets* a, int32_t cookie) {
    return cookie >= 1 && (uint32_t)cookie <= a->table_count ? &a->tables[cookie - 1].strings : NULL;
}

/* ---- configurations ---- */

/* a type chunk's ResTable_config, the fields it has */
typedef struct {
    uint16_t mcc, mnc, density, sdk, width_dp, height_dp, smallest_dp;
    char language[2], country[2];
    uint8_t orientation, touchscreen, keyboard, navigation, screen_layout, ui_mode;
} Config;
static Config chunk_config(const unsigned char* chunk) {
    const unsigned char* c = chunk + 20;
    uint32_t size = ld32(c);
    unsigned char b[64] = {0};
    memcpy(b, c, size < sizeof b ? size : sizeof b);
    Config k = {0};
    k.mcc = ld16(b + 4);
    k.mnc = ld16(b + 6);
    memcpy(k.language, b + 8, 2);
    memcpy(k.country, b + 10, 2);
    k.orientation = b[12];
    k.touchscreen = b[13];
    k.density = ld16(b + 14);
    k.keyboard = b[16];
    k.navigation = b[17];
    k.sdk = ld16(b + 24);
    k.screen_layout = b[28];
    k.ui_mode = b[29];
    k.smallest_dp = ld16(b + 30);
    k.width_dp = ld16(b + 32);
    k.height_dp = ld16(b + 34);
    return k;
}
static bool matches(const Config* k, const ArscConfig* d) {
    if (k->mcc || k->mnc) return false; /* no SIM */
    if (k->language[0] && (k->language[0] != d->language[0] || k->language[1] != d->language[1])) return false;
    if (k->country[0] && (k->country[0] != d->country[0] || k->country[1] != d->country[1])) return false;
    if (k->orientation && d->orientation && k->orientation != d->orientation) return false;
    if (k->sdk && k->sdk > d->sdk) return false;
    if ((k->ui_mode & 0x30) && (k->ui_mode & 0x30) != (d->ui_mode & 0x30)) return false;
    if ((k->ui_mode & 0x0f) && (k->ui_mode & 0x0f) != (d->ui_mode & 0x0f)) return false;
    if (k->smallest_dp && k->smallest_dp > d->smallest_width_dp) return false;
    if (k->width_dp && k->width_dp > d->width_dp) return false;
    if (k->height_dp && k->height_dp > d->height_dp) return false;
    if ((k->screen_layout & 0x0f) && (k->screen_layout & 0x0f) > (d->screen_layout & 0x0f)) return false;
    if ((k->screen_layout & 0x30) && (k->screen_layout & 0x30) != (d->screen_layout & 0x30)) return false;
    if ((k->screen_layout & 0xc0) && (k->screen_layout & 0xc0) != (d->screen_layout & 0xc0)) return false;
    if (k->touchscreen && d->touchscreen && k->touchscreen != d->touchscreen) return false;
    if (k->keyboard && d->keyboard && k->keyboard != d->keyboard) return false;
    if (k->navigation && d->navigation && k->navigation != d->navigation) return false;
    return true;
}
/* the cost of a density for the device's: exact is best, then higher (scaled down), then lower */
static uint32_t density_cost(uint16_t density, uint16_t device) {
    if (density == 0xfffe) return 0; /* anydpi */
    uint32_t d = density ? density : 160, want = device ? device : 160;
    return d >= want ? d - want : (want - d) * 4;
}
/* whether a is a better match than b for the device, in AOSP's order of qualifiers */
static bool better(const Config* a, const Config* b, const ArscConfig* d) {
#define PREFER(x, y) \
    if ((x) != (y)) return (x) > (y)
    PREFER(a->language[0] != 0, b->language[0] != 0);
    PREFER(a->country[0] != 0, b->country[0] != 0);
    PREFER((a->screen_layout & 0xc0) != 0, (b->screen_layout & 0xc0) != 0);
    PREFER(a->smallest_dp, b->smallest_dp);
    PREFER(a->width_dp, b->width_dp);
    PREFER(a->height_dp, b->height_dp);
    PREFER(a->screen_layout & 0x0f, b->screen_layout & 0x0f);
    PREFER((a->screen_layout & 0x30) != 0, (b->screen_layout & 0x30) != 0);
    PREFER(a->orientation != 0, b->orientation != 0);
    PREFER((a->ui_mode & 0x0f) != 0, (b->ui_mode & 0x0f) != 0);
    PREFER((a->ui_mode & 0x30) != 0, (b->ui_mode & 0x30) != 0);
    if (a->density != b->density) return density_cost(a->density, d->density) < density_cost(b->density, d->density);
    PREFER(a->touchscreen != 0, b->touchscreen != 0);
    PREFER(a->keyboard != 0, b->keyboard != 0);
    PREFER(a->navigation != 0, b->navigation != 0);
    PREFER(a->sdk, b->sdk);
#undef PREFER
    return false;
}

/* ---- entries ---- */

/* entry index's offset in a type chunk, NO_ENTRY for none */
static uint32_t entry_offset(const unsigned char* chunk, uint32_t index) {
    uint32_t header = ld16(chunk + 2), size = ld32(chunk + 4), count = ld32(chunk + 12), start = ld32(chunk + 16);
    unsigned flags = chunk[9];
    const unsigned char* offsets = chunk + header;
    uint32_t offset;
    if (flags & TYPE_SPARSE) {
        if ((uint64_t)header + 4ull * count > size) return NO_ENTRY;
        uint32_t low = 0, high = count;
        offset = NO_ENTRY;
        while (low < high) {
            uint32_t mid = low + (high - low) / 2, idx = ld16(offsets + 4 * mid);
            if (idx == index) {
                offset = ld16(offsets + 4 * mid + 2) * 4u;
                break;
            }
            if (idx < index)
                low = mid + 1;
            else
                high = mid;
        }
        if (offset == NO_ENTRY) return NO_ENTRY;
    } else if (flags & TYPE_OFFSET16) {
        if (index >= count || (uint64_t)header + 2ull * count > size) return NO_ENTRY;
        uint32_t o = ld16(offsets + 2 * index);
        if (o == 0xffff) return NO_ENTRY;
        offset = o * 4;
    } else {
        if (index >= count || (uint64_t)header + 4ull * count > size) return NO_ENTRY;
        offset = ld32(offsets + 4 * index);
        if (offset == NO_ENTRY) return NO_ENTRY;
    }
    uint64_t at = (uint64_t)start + offset;
    return at + 8 <= size ? (uint32_t)at : NO_ENTRY;
}
/* a found entry */
typedef struct {
    const Table* table;
    const Package* package;
    const unsigned char* chunk;
    const unsigned char* entry;
    int32_t cookie;
    uint32_t spec_flags;
    uint16_t density;
} Found;
static bool find_package(ArscAssets* a, uint32_t id, Found* f, const Package** package) {
    for (uint32_t t = 0; t < a->table_count; ++t)
        for (uint32_t p = 0; p < a->tables[t].package_count; ++p)
            if (a->tables[t].packages[p].id == id) {
                f->table = &a->tables[t];
                f->cookie = (int32_t)t + 1;
                *package = &a->tables[t].packages[p];
                return true;
            }
    return false;
}
static bool find_entry(ArscAssets* a, uint32_t resid, Found* f) {
    *f = (Found){0};
    const Package* p;
    if (!find_package(a, resid >> 24, f, &p)) return false;
    unsigned type = (resid >> 16) & 0xff;
    uint32_t index = resid & 0xffff;
    const Type* t = &p->types[type];
    Config best = {0};
    for (uint32_t i = 0; i < t->chunk_count; ++i) {
        const unsigned char* chunk = t->chunks[i];
        Config k = chunk_config(chunk);
        if (!matches(&k, &a->config)) continue;
        uint32_t offset = entry_offset(chunk, index);
        if (offset == NO_ENTRY) continue;
        if (f->entry && !better(&k, &best, &a->config)) continue;
        best = k;
        f->chunk = chunk;
        f->entry = chunk + offset;
    }
    if (!f->entry) return false;
    f->package = p;
    f->spec_flags = t->spec && index < t->spec_count ? ld32(t->spec + 4 * index) : 0;
    f->density = best.density;
    return true;
}
static bool entry_complex(const Found* f) {
    unsigned flags = ld16(f->entry + 2);
    return !(flags & ENTRY_COMPACT) && (flags & ENTRY_COMPLEX);
}
static ResValue entry_value(const Found* f) {
    unsigned flags = ld16(f->entry + 2);
    if (flags & ENTRY_COMPACT) return (ResValue){(uint8_t)(flags >> 8), ld32(f->entry + 4)};
    uint32_t size = ld16(f->entry);
    const unsigned char* v = f->entry + size;
    const unsigned char* end = f->chunk + ld32(f->chunk + 4);
    if (v + 8 > end) return (ResValue){RES_TYPE_NULL, 0};
    return (ResValue){v[3], ld32(v + 4)};
}
static uint32_t entry_key(const Found* f) {
    unsigned flags = ld16(f->entry + 2);
    return flags & ENTRY_COMPACT ? ld16(f->entry) : ld32(f->entry + 4);
}

bool arsc_get(ArscAssets* a, uint32_t resid, ArscValue* out) {
    Found f;
    if (!find_entry(a, resid, &f) || entry_complex(&f)) return false;
    *out = (ArscValue){entry_value(&f), f.cookie, resid, f.spec_flags, f.density};
    return true;
}

bool arsc_resolve_reference(ArscAssets* a, ArscValue* v) {
    for (int depth = 0; depth < MAX_DEPTH; ++depth) {
        if ((v->value.type != RES_TYPE_REFERENCE && v->value.type != RES_TYPE_DYNAMIC_REFERENCE) || !v->value.data)
            return true;
        uint32_t resid = v->value.data;
        Found f;
        if (!find_entry(a, resid, &f)) { /* the reference stays, naming what it refers to, as Android's */
            v->resid = resid;
            return false;
        }
        if (entry_complex(&f)) { /* a bag: the reference stays, naming it */
            v->resid = resid;
            v->flags |= f.spec_flags;
            v->cookie = f.cookie;
            return true;
        }
        v->value = entry_value(&f);
        v->cookie = f.cookie;
        v->resid = resid;
        v->flags |= f.spec_flags;
        v->density = f.density;
    }
    return false;
}

/* ---- bags ---- */

static Bag* bag_slot(ArscAssets* a, uint32_t resid) {
    if (!a->bag_capacity) return NULL;
    for (uint32_t i = resid * 2654435761u & (a->bag_capacity - 1);; i = (i + 1) & (a->bag_capacity - 1))
        if (a->bags[i].resid == resid || !a->bags[i].resid) return &a->bags[i];
}
static Bag* bag_insert(ArscAssets* a, uint32_t resid) {
    if ((a->bag_count + 1) * 2 > a->bag_capacity) {
        uint32_t capacity = a->bag_capacity ? a->bag_capacity * 2 : 256;
        Bag* bags = calloc(capacity, sizeof *bags);
        if (!bags) return NULL;
        Bag* old = a->bags;
        uint32_t old_capacity = a->bag_capacity;
        a->bags = bags;
        a->bag_capacity = capacity;
        for (uint32_t i = 0; i < old_capacity; ++i)
            if (old[i].resid) *bag_slot(a, old[i].resid) = old[i];
        free(old);
    }
    Bag* b = bag_slot(a, resid);
    if (!b->resid) {
        b->resid = resid;
        ++a->bag_count;
    }
    return b;
}
/* the merged bag of resid: its parent's entries, then its own over them */
static const Bag* make_bag(ArscAssets* a, uint32_t resid, int depth) {
    Bag* cached = bag_slot(a, resid);
    if (cached && cached->resid == resid) return cached;
    Found f;
    if (depth > MAX_DEPTH || !find_entry(a, resid, &f) || !entry_complex(&f)) return NULL;
    uint32_t size = ld16(f.entry), parent = ld32(f.entry + 8), count = ld32(f.entry + 12);
    const unsigned char* maps = f.entry + size;
    const unsigned char* end = f.chunk + ld32(f.chunk + 4);
    if ((uint64_t)(end - maps) < 12ull * count) count = (uint32_t)((end - maps) / 12);
    const Bag* base = parent ? make_bag(a, parent, depth + 1) : NULL;
    uint32_t base_count = base ? base->count : 0;
    uint32_t* keys = malloc(((size_t)base_count + count + 1) * sizeof *keys);
    ArscValue* values = malloc(((size_t)base_count + count + 1) * sizeof *values);
    if (!keys || !values) {
        free(keys);
        free(values);
        return NULL;
    }
    uint32_t n = base_count;
    if (base_count) {
        memcpy(keys, base->keys, base_count * sizeof *keys);
        memcpy(values, base->values, base_count * sizeof *values);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* m = maps + 12 * i;
        uint32_t key = ld32(m);
        ArscValue v = {{m[7], ld32(m + 8)}, f.cookie, 0, f.spec_flags, f.density};
        /* the key's place in the sorted keys: replaced, or made room for */
        uint32_t low = 0, high = n;
        while (low < high) {
            uint32_t mid = low + (high - low) / 2;
            if (keys[mid] < key)
                low = mid + 1;
            else
                high = mid;
        }
        if (low < n && keys[low] == key) {
            values[low] = v;
        } else {
            memmove(keys + low + 1, keys + low, (n - low) * sizeof *keys);
            memmove(values + low + 1, values + low, (n - low) * sizeof *values);
            keys[low] = key;
            values[low] = v;
            ++n;
        }
    }
    /* the insert may move the cache's other bags, and base with them: it is not used after this */
    Bag* b = bag_insert(a, resid);
    if (!b) {
        free(keys);
        free(values);
        return NULL;
    }
    b->keys = keys;
    b->values = values;
    b->count = n;
    return b;
}
bool arsc_bag(ArscAssets* a, uint32_t resid, const uint32_t** keys, const ArscValue** values, uint32_t* count) {
    const Bag* b = make_bag(a, resid, 0);
    *keys = b ? b->keys : NULL;
    *values = b ? b->values : NULL;
    *count = b ? b->count : 0;
    return b != NULL;
}
/* an attribute's value in a bag, NULL for none */
static const ArscValue* bag_find(const uint32_t* keys, const ArscValue* values, uint32_t count, uint32_t key) {
    uint32_t low = 0, high = count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (keys[mid] == key) return &values[mid];
        if (keys[mid] < key)
            low = mid + 1;
        else
            high = mid;
    }
    return NULL;
}

/* ---- names ---- */

bool arsc_name(ArscAssets* a, uint32_t resid, char* package, size_t package_size, char* type, size_t type_size,
               char* entry, size_t entry_size) {
    Found f;
    if (!find_entry(a, resid, &f)) {
        /* another configuration's entry still names it */
        ArscConfig saved = a->config;
        ArscConfig any = {0};
        any.sdk = 0xffff;
        any.width_dp = any.height_dp = any.smallest_width_dp = 0xffff;
        any.screen_layout = 0xff;
        a->config = any;
        bool found = find_entry(a, resid, &f);
        a->config = saved;
        if (!found) return false;
    }
    snprintf(package, package_size, "%s", f.package->name);
    if (res_string(&f.package->type_names, ((resid >> 16) & 0xff) - 1, type, type_size) < 0) return false;
    return res_string(&f.package->keys, entry_key(&f), entry, entry_size) >= 0;
}
/* a string's index in a pool, by a scan; -1 for none */
static long pool_find(const ResStringPool* pool, const char* text) {
    char buffer[512];
    for (uint32_t i = 0; i < pool->count; ++i)
        if (res_string(pool, i, buffer, sizeof buffer) >= 0 && !strcmp(buffer, text)) return (long)i;
    return -1;
}
uint32_t arsc_identifier(ArscAssets* a, const char* package, const char* type, const char* entry) {
    for (uint32_t t = 0; t < a->table_count; ++t)
        for (uint32_t p = 0; p < a->tables[t].package_count; ++p) {
            const Package* pk = &a->tables[t].packages[p];
            if (package && package[0] && strcmp(pk->name, package)) continue;
            long type_index = pool_find(&pk->type_names, type), key = pool_find(&pk->keys, entry);
            if (type_index < 0 || key < 0 || type_index + 1 >= MAX_TYPES) continue;
            const Type* ty = &pk->types[type_index + 1];
            for (uint32_t c = 0; c < ty->chunk_count; ++c) {
                const unsigned char* chunk = ty->chunks[c];
                uint32_t count = ld32(chunk + 12);
                if (chunk[9] & TYPE_SPARSE) count = 0x10000;
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t offset = entry_offset(chunk, i);
                    if (offset == NO_ENTRY) continue;
                    Found f = {0};
                    f.chunk = chunk;
                    f.entry = chunk + offset;
                    if (entry_key(&f) == (uint32_t)key) return pk->id << 24 | (uint32_t)(type_index + 1) << 16 | i;
                }
            }
        }
    return 0;
}

/* ---- themes ---- */

ArscTheme* arsc_theme_create(ArscAssets* assets) {
    ArscTheme* t = calloc(1, sizeof *t);
    if (t) t->assets = assets;
    return t;
}
void arsc_theme_destroy(ArscTheme* t) {
    if (!t) return;
    free(t->entries);
    free(t->styles);
    free(t);
}
ArscAssets* arsc_theme_assets(const ArscTheme* t) {
    return t->assets;
}
void arsc_theme_clear(ArscTheme* t) {
    t->count = 0;
    t->style_count = 0;
}
static ThemeEntry* theme_find(const ArscTheme* t, uint32_t attr) {
    uint32_t low = 0, high = t->count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (t->entries[mid].attr == attr) return &t->entries[mid];
        if (t->entries[mid].attr < attr)
            low = mid + 1;
        else
            high = mid;
    }
    return NULL;
}
void arsc_theme_apply(ArscTheme* t, uint32_t style, bool force) {
    const uint32_t* keys;
    const ArscValue* values;
    uint32_t count;
    if (!arsc_bag(t->assets, style, &keys, &values, &count)) return;
    if (t->style_count == t->style_capacity) {
        uint32_t capacity = t->style_capacity ? t->style_capacity * 2 : 8;
        uint32_t* styles = realloc(t->styles, capacity * sizeof *styles);
        if (!styles) return;
        t->styles = styles;
        t->style_capacity = capacity;
    }
    t->styles[t->style_count++] = style | (force ? 0x80000000u : 0) /* never a package ID's top bit */;
    /* a merge of two sorted lists: the style's entries replace the theme's only when forced */
    ThemeEntry* merged = malloc(((size_t)t->count + count + 1) * sizeof *merged);
    if (!merged) return;
    uint32_t i = 0, j = 0, n = 0;
    while (i < t->count || j < count) {
        if (j >= count || (i < t->count && t->entries[i].attr < keys[j])) {
            merged[n++] = t->entries[i++];
        } else if (i >= t->count || keys[j] < t->entries[i].attr) {
            merged[n++] = (ThemeEntry){keys[j], values[j]};
            ++j;
        } else {
            merged[n++] = force ? (ThemeEntry){keys[j], values[j]} : t->entries[i];
            ++i;
            ++j;
        }
    }
    free(t->entries);
    t->entries = merged;
    t->count = t->capacity = n;
}
void arsc_theme_copy(ArscTheme* dst, const ArscTheme* src) {
    ThemeEntry* entries = malloc(((size_t)src->count + 1) * sizeof *entries);
    uint32_t* styles = malloc(((size_t)src->style_count + 1) * sizeof *styles);
    if (!entries || !styles) {
        free(entries);
        free(styles);
        return;
    }
    if (src->count) memcpy(entries, src->entries, src->count * sizeof *entries);
    if (src->style_count) memcpy(styles, src->styles, src->style_count * sizeof *styles);
    free(dst->entries);
    free(dst->styles);
    dst->entries = entries;
    dst->count = dst->capacity = src->count;
    dst->styles = styles;
    dst->style_count = dst->style_capacity = src->style_count;
}
uint32_t arsc_theme_styles(const ArscTheme* t, uint32_t* styles, bool* forced, uint32_t max) {
    for (uint32_t i = 0; i < t->style_count && i < max; ++i) {
        styles[i] = t->styles[i] & 0x7fffffffu;
        forced[i] = (t->styles[i] & 0x80000000u) != 0;
    }
    return t->style_count;
}
bool arsc_theme_get(ArscTheme* t, uint32_t attr, ArscValue* out) {
    uint32_t flags = 0;
    for (int depth = 0; depth < MAX_DEPTH; ++depth) {
        const ThemeEntry* e = theme_find(t, attr);
        if (!e) return false;
        flags |= e->value.flags;
        if (e->value.value.type == RES_TYPE_ATTRIBUTE) {
            attr = e->value.value.data;
            continue;
        }
        *out = e->value;
        out->flags = flags;
        return e->value.value.type != RES_TYPE_NULL || e->value.value.data == 1 /* DATA_NULL_EMPTY */;
    }
    return false;
}
bool arsc_theme_resolve(ArscTheme* t, ArscValue* v) {
    if (v->value.type == RES_TYPE_ATTRIBUTE || v->value.type == RES_TYPE_DYNAMIC_ATTRIBUTE) {
        ArscValue found;
        if (!arsc_theme_get(t, v->value.data, &found)) return false;
        uint32_t flags = v->flags;
        *v = found;
        v->flags |= flags;
    }
    return arsc_resolve_reference(t->assets, v);
}

/* ---- attribute retrieval ---- */

static void write_entry(int32_t* out, const ArscValue* v, uint32_t source) {
    out[0] = v->value.type;
    out[1] = (int32_t)v->value.data;
    out[2] = v->cookie;
    out[3] = (int32_t)v->resid;
    out[4] = (int32_t)v->flags;
    out[5] = v->density;
    out[6] = (int32_t)source;
}
/* a style's bag, or none */
typedef struct {
    const uint32_t* keys;
    const ArscValue* values;
    uint32_t count, resid;
} Style;
static Style style_of(ArscAssets* a, uint32_t resid) {
    Style s = {NULL, NULL, 0, resid};
    if (resid) arsc_bag(a, resid, &s.keys, &s.values, &s.count);
    return s;
}
/* the merged bags may move when another is made: each style's is looked up again after the last is made */
static const ArscValue* style_find(ArscAssets* a, Style* s, uint32_t attr) {
    if (!s->resid) return NULL;
    arsc_bag(a, s->resid, &s->keys, &s->values, &s->count);
    return bag_find(s->keys, s->values, s->count, attr);
}
/* the finished value of an attribute: a theme attribute or a reference resolved; false when it has none */
static bool finish(ArscTheme* t, ArscValue* v) {
    /* @null, which aapt writes as a reference to 0: no value, as Android's attribute resolution makes it */
    if (v->value.type == RES_TYPE_REFERENCE && !v->value.data) return false;
    if (v->value.type == RES_TYPE_NULL && v->value.data != 1) return false;
    if (!t) return true;
    if (v->value.type == RES_TYPE_ATTRIBUTE || v->value.type == RES_TYPE_DYNAMIC_ATTRIBUTE) {
        ArscValue found;
        if (!arsc_theme_get(t, v->value.data, &found)) return false;
        uint32_t flags = v->flags;
        *v = found;
        v->flags |= flags;
    }
    /* a reference that does not resolve (to the framework's resources, which the tables lack, as
       @android:id/button1) stays the reference, as Android's ApplyStyle keeps it */
    arsc_resolve_reference(t->assets, v);
    return v->value.type != RES_TYPE_NULL || v->value.data == 1;
}
void arsc_apply_style(ArscTheme* t, const AxmlParser* xml, uint32_t def_style_attr, uint32_t def_style_res,
                      const uint32_t* attrs, uint32_t count, int32_t* out, int32_t* indices) {
    ArscAssets* a = t->assets;
    if (def_style_attr) {
        ArscValue v;
        if (arsc_theme_get(t, def_style_attr, &v) && v.value.type == RES_TYPE_REFERENCE) def_style_res = v.value.data;
    }
    uint32_t xml_style = 0;
    if (xml) {
        long i = axml_style_attribute(xml);
        if (i >= 0) {
            ArscValue v = {axml_attribute_value(xml, (uint32_t)i), 0, 0, 0, 0};
            if (v.value.type == RES_TYPE_ATTRIBUTE) arsc_theme_resolve(t, &v);
            if (v.value.type == RES_TYPE_REFERENCE) xml_style = v.value.data;
        }
    }
    Style style = style_of(a, xml_style), def = style_of(a, def_style_res);
    uint32_t found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        ArscValue v = {{RES_TYPE_NULL, 0}, -1, 0, 0, 0};
        uint32_t source = 0;
        bool have = false;
        long at = xml ? axml_find_resource(xml, attrs[i]) : -1;
        const ArscValue* s;
        if (at >= 0) {
            v.value = axml_attribute_value(xml, (uint32_t)at);
            v.cookie = -1;
            have = v.value.type != RES_TYPE_NULL || v.value.data == 1;
        }
        if (!have && (s = style_find(a, &style, attrs[i]))) {
            v = *s;
            source = style.resid;
            have = true;
        }
        if (!have && (s = style_find(a, &def, attrs[i]))) {
            v = *s;
            source = def.resid;
            have = true;
        }
        if (!have) have = arsc_theme_get(t, attrs[i], &v);
        int32_t* o = out + (size_t)i * ARSC_STYLE_ENTRIES;
        if (have && finish(t, &v)) {
            if (v.cookie == -1 && v.value.type != RES_TYPE_STRING) v.cookie = 0;
            write_entry(o, &v, source);
            indices[++found] = (int32_t)i;
        } else {
            memset(o, 0, ARSC_STYLE_ENTRIES * sizeof *o);
        }
    }
    indices[0] = (int32_t)found;
}
bool arsc_resolve_attrs(ArscTheme* t, uint32_t def_style_attr, uint32_t def_style_res, const int32_t* in_values,
                        uint32_t in_count, const uint32_t* attrs, uint32_t count, int32_t* out, int32_t* indices) {
    ArscAssets* a = t->assets;
    if (def_style_attr) {
        ArscValue v;
        if (arsc_theme_get(t, def_style_attr, &v) && v.value.type == RES_TYPE_REFERENCE) def_style_res = v.value.data;
    }
    Style def = style_of(a, def_style_res);
    uint32_t found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        ArscValue v = {{RES_TYPE_NULL, 0}, 0, 0, 0, 0};
        uint32_t source = 0;
        bool have = false;
        const ArscValue* s;
        if (i < in_count && in_values && in_values[i]) {
            v.value = (ResValue){RES_TYPE_ATTRIBUTE, (uint32_t)in_values[i]};
            have = true;
        }
        if (!have && (s = style_find(a, &def, attrs[i]))) {
            v = *s;
            source = def.resid;
            have = true;
        }
        if (!have) have = arsc_theme_get(t, attrs[i], &v);
        int32_t* o = out + (size_t)i * ARSC_STYLE_ENTRIES;
        if (have && finish(t, &v)) {
            write_entry(o, &v, source);
            if (indices) indices[++found] = (int32_t)i;
        } else {
            memset(o, 0, ARSC_STYLE_ENTRIES * sizeof *o);
        }
    }
    if (indices) indices[0] = (int32_t)found;
    return true;
}
bool arsc_retrieve_attributes(ArscAssets* a, const AxmlParser* xml, const uint32_t* attrs, uint32_t count,
                              int32_t* out, int32_t* indices) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t* o = out + (size_t)i * ARSC_STYLE_ENTRIES;
        long at = xml ? axml_find_resource(xml, attrs[i]) : -1;
        ArscValue v = {{RES_TYPE_NULL, 0}, -1, 0, 0, 0};
        if (at >= 0) v.value = axml_attribute_value(xml, (uint32_t)at);
        /* @null (a reference to 0): no value */
        if (at >= 0 && v.value.type == RES_TYPE_REFERENCE && !v.value.data) v.value = (ResValue){RES_TYPE_NULL, 0};
        if (at >= 0 && v.value.type == RES_TYPE_REFERENCE) arsc_resolve_reference(a, &v);
        if (at >= 0 && (v.value.type != RES_TYPE_NULL || v.value.data == 1)) {
            if (v.cookie == -1 && v.value.type != RES_TYPE_STRING) v.cookie = 0;
            write_entry(o, &v, 0);
            if (indices) indices[++found] = (int32_t)i;
        } else {
            memset(o, 0, ARSC_STYLE_ENTRIES * sizeof *o);
        }
    }
    if (indices) indices[0] = (int32_t)found;
    return true;
}
