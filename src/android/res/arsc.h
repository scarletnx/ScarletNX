/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_ARSC_H
#define SNX_ANDROID_ARSC_H
#include "android/res/axml.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* resources.arsc, the HLE of libandroidfw's AssetManager2 and Theme: the resource tables of the loaded APKs, the
   lookup of a resource ID for the device's configuration, style bags with their parents, and themes. The format is
   AOSP's ResourceTypes.h. Every function of an AssetManager must be called by one thread at a time (the Java side
   holds its lock) */

/* the device configuration resources are chosen for (ResTable_config's fields this matches) */
typedef struct {
    char language[4], country[4]; /* "en", "US"; empty for none */
    uint16_t density;             /* dpi */
    uint16_t sdk;
    uint8_t orientation;          /* 1 portrait, 2 landscape */
    uint8_t ui_mode;              /* type in the low 4 bits, night (0x20) or not night (0x10) above */
    uint16_t width_dp, height_dp, smallest_width_dp;
    uint8_t screen_layout;        /* size (low 4 bits), long (0x30 mask), layout direction (0xc0 mask) */
    uint8_t touchscreen, keyboard, navigation;
} ArscConfig;

/* a value found for a resource: the table it came from (its cookie, 1 for the first), the resource that held it,
   its type spec flags (the configuration changes it depends on) and its density */
typedef struct {
    ResValue value;
    int32_t cookie;
    uint32_t resid;
    uint32_t flags;
    uint16_t density;
} ArscValue;

typedef struct ArscAssets ArscAssets;
typedef struct ArscTheme ArscTheme;

ArscAssets* arsc_create(void);
void arsc_destroy(ArscAssets* assets);
/* a resources.arsc, whose bytes the assets keep (copied): its cookie, or 0 with error filled */
int32_t arsc_add(ArscAssets* assets, const unsigned char* data, size_t size, char* error, size_t error_size);
void arsc_set_config(ArscAssets* assets, const ArscConfig* config);
/* the string pool of a cookie's table, NULL for a bad cookie */
const ResStringPool* arsc_strings(const ArscAssets* assets, int32_t cookie);

/* the value of resid for the configuration; false for none (a missing resource, a bag) */
bool arsc_get(ArscAssets* assets, uint32_t resid, ArscValue* out);
/* whether resid is a bag (a style, an array, plurals), and its entries with its parents': count of them into
   attrs and values (sorted by attr), NULL count 0 for none. The assets keep them until the configuration changes */
bool arsc_bag(ArscAssets* assets, uint32_t resid, const uint32_t** keys, const ArscValue** values, uint32_t* count);
/* a reference followed until it is not one (or a bag's): out changed in place; false for one that leads nowhere */
bool arsc_resolve_reference(ArscAssets* assets, ArscValue* value);

/* the name of a resource: its package, type and entry names as UTF-8, false for an unknown ID */
bool arsc_name(ArscAssets* assets, uint32_t resid, char* package, size_t package_size, char* type, size_t type_size,
               char* entry, size_t entry_size);
/* the ID of package:type/entry, 0 for none */
uint32_t arsc_identifier(ArscAssets* assets, const char* package, const char* type, const char* entry);

/* themes */
ArscTheme* arsc_theme_create(ArscAssets* assets);
void arsc_theme_destroy(ArscTheme* theme);
ArscAssets* arsc_theme_assets(const ArscTheme* theme);
void arsc_theme_apply(ArscTheme* theme, uint32_t style, bool force);
void arsc_theme_copy(ArscTheme* dst, const ArscTheme* src);
void arsc_theme_clear(ArscTheme* theme);
/* an attribute's value in the theme, ?attr references followed; false for none */
bool arsc_theme_get(ArscTheme* theme, uint32_t attr, ArscValue* out);
/* an attribute reference (TYPE_ATTRIBUTE) or a resource reference resolved through the theme and the tables */
bool arsc_theme_resolve(ArscTheme* theme, ArscValue* value);
/* the applied styles' IDs and whether each was forced, count into the arrays (at most max); the total count */
uint32_t arsc_theme_styles(const ArscTheme* theme, uint32_t* styles, bool* forced, uint32_t max);

/* AOSP's attribute retrieval, STYLE_NUM_ENTRIES (7) ints for each attribute in out: type, data, cookie, resource,
   changing configurations, density, source resource. indices gets the count, then the indices of the attributes
   that have a value. A value from xml has cookie -1, its string in the xml's pool */
enum { ARSC_STYLE_ENTRIES = 7 };
void arsc_apply_style(ArscTheme* theme, const AxmlParser* xml, uint32_t def_style_attr, uint32_t def_style_res,
                      const uint32_t* attrs, uint32_t count, int32_t* out, int32_t* indices);
bool arsc_resolve_attrs(ArscTheme* theme, uint32_t def_style_attr, uint32_t def_style_res, const int32_t* in_values,
                        uint32_t in_count, const uint32_t* attrs, uint32_t count, int32_t* out, int32_t* indices);
bool arsc_retrieve_attributes(ArscAssets* assets, const AxmlParser* xml, const uint32_t* attrs, uint32_t count,
                              int32_t* out, int32_t* indices);
#endif
