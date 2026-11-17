/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's languages. Its phrases are English in the code, each through snx_tr; scripts/build-languages.py lists
   them in po/scarletnx.pot and makes each po/<language>.po a catalog (the .mo format) compiled into the menu. A phrase
   a language has no translation for stays English. Strict ISO C23, no SDL: main.c asks the system for its locales. */
#ifndef SNX_GUI_I18N_H
#define SNX_GUI_I18N_H
#include <stddef.h>

/* Marks for the script, the phrase itself in the code. A table's phrases are marked where they are written and go
   through snx_tr where they are shown. SNX_TR_KEYS: a foot's keys ("Key: what it does." sentences), whose
   translation keeps each key or the key's own translation; SNX_TR_KEY: such a key, which a click on the foot finds
   by its English or its translation */
#define SNX_TR_NOOP(s) s
#define SNX_TR_KEYS(s) s
#define SNX_TR_KEY(s)  s

typedef struct {
    const char* code; /* the .po's name, a BCP 47 tag (common/utilities/language_tag.h): "pt-BR"; "en" for English */
    const char* name; /* in the language itself, UTF-8 */
    const unsigned char* mo;
    size_t size; /* 0: no catalog (English) */
} SnxLanguage;
/* the menu's, English first: generated/gui_languages.c, from po/ */
extern const SnxLanguage snx_gui_languages[];
extern const int snx_gui_language_count;

/* the languages snx_tr picks from, list[0] English; English in use until snx_tr_use */
void snx_tr_languages(const SnxLanguage* list, int count);
/* list[language] from now on; a language past the list is English */
void snx_tr_use(int language);
int snx_tr_language(void);
/* english in the language in use, english itself when the language has no translation of it. A format's arguments
   are checked against english, which the translation keeps (scripts/build-languages.py) */
#if defined(__GNUC__) || defined(__clang__)
#define SNX_TR_FORMAT __attribute__((format_arg(1)))
#else
#define SNX_TR_FORMAT
#endif
const char* snx_tr(const char* english) SNX_TR_FORMAT;
/* the language code names: its tag, or the tag in another form (pt_BR, zh-CN for zh-Hans-CN). -1 for none */
int snx_tr_find(const char* code);
/* the language of the first of the user's locales (pt_BR.UTF-8, pt, zh-Hant_TW, the most wanted first) one of the
   list's is for, as snx_language_fit has it: the same language, script and region, else the language with no region,
   else the language for another region; never one of another script. English (0) when none is */
int snx_tr_pick(const char* const* locales, int count);
/* musl's __mo_lookup: the translation of s in a catalog of size bytes, NULL when it has none or the catalog is not a
   whole one */
const char* snx_mo_lookup(const unsigned char* mo, size_t size, const char* s);
#endif
