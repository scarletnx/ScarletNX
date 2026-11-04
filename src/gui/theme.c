/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/theme.h"
#include "gui/i18n.h"
#include "gui/model.h" /* GUI_TEXT_MAX */
#include "gui/steps.h" /* GuiProgress */
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "gui_themes.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the built-ins: each folder of src/gui/themes, its theme.ini compiled in (cmake/gui_themes.cmake) and read the
   first time one is asked for. Their files are their only source: nothing here holds a value of theirs */
static GuiTheme builtins[SNX_GUI_THEME_COUNT];
/* a theme's id fits a slot of the theme list, and a word's field is an int: an array of negative size otherwise */
typedef char snx_gui_id_slot[sizeof(((GuiThemes*)0)->ids[0]) == sizeof(((GuiTheme*)0)->id) ? 1 : -1];
typedef char snx_gui_word_int[sizeof(GuiStyle) == sizeof(int) && sizeof(GuiRunLayout) == sizeof(int) &&
                                      sizeof(GuiRing) == sizeof(int) && sizeof(GuiLineIn) == sizeof(int) &&
                                      sizeof(GuiAlign) == sizeof(int) && sizeof(GuiPanels) == sizeof(int) &&
                                      sizeof(GuiDialog) == sizeof(int) && sizeof(GuiPicture) == sizeof(int)
                                  ? 1
                                  : -1];

static const char* const heads[] = {"plain", "bar", NULL};
static const char* const styles[] = {"plain", "bar", "none", NULL};
static const char* const focuses[] = {"color", "bar", NULL};
static const char* const layouts[] = {"console", "steps", NULL};
static const char* const rings[] = {"none", "left", "right", "right-near-text", NULL};
static const char* const progresses[] = {"amount", "percent", "time", NULL}; /* GuiProgress's order */
static const char* const aligns[] = {"left", "center", "right", NULL};
static const char* const entrances[] = {"none", "typing", "slide", "fade", NULL};
static const char* const typings[] = {"async", "sync", NULL};
static const char* const panel_words[] = {"outline", "fill", NULL};
static const char* const flows[] = {"wrap", "slide", NULL};
static const char* const pictures[] = {"none", "square", "round", NULL}; /* GuiPicture's order */
static const char* const dialogs[] = {"default", "yorha", NULL}; /* GuiDialog's order */
#define FIELD(f)                    offsetof(GuiTheme, f), sizeof(((GuiTheme*)0)->f)
#define TEXT(s, k, f, about)        {s, k, GUI_KEY_TEXT, FIELD(f), NULL, 0, about, 0, 0, 0, 0, NULL}
#define COLOUR(s, k, f, about)      {s, k, GUI_KEY_COLOUR, FIELD(f), NULL, 0, about, 0, 0, 0, 0, NULL}
#define FLAG(s, k, f, about)        {s, k, GUI_KEY_FLAG, FIELD(f), NULL, 0, about, 0, 0, 0, 0, NULL}
#define NUMBER(s, k, f, max, about) {s, k, GUI_KEY_NUMBER, FIELD(f), NULL, max, about, 0, 0, 0, 0, NULL}
#define WORD(s, k, f, words, about) {s, k, GUI_KEY_WORD, FIELD(f), words, 0, about, 0, 0, 0, 0, NULL}
const GuiThemeKey gui_theme_keys[] = {
    TEXT("theme", "name", name, SNX_TR_NOOP("the name the menu's Theme item shows")),
    TEXT("theme", "author", author, SNX_TR_NOOP("who made it")),
    COLOUR("colors", "page", page, SNX_TR_NOOP("the background")),
    COLOUR("colors", "ink", ink, SNX_TR_NOOP("text; the head's and footer's bar")),
    COLOUR("colors", "soft", soft, SNX_TR_NOOP("hints, secondary text, rules")),
    COLOUR("colors", "focus", focus, SNX_TR_NOOP("the focused line, or its band")),
    COLOUR("colors", "active", active, SNX_TR_NOOP("the item chosen, the run's head")),
    COLOUR("colors", "failure", failure, SNX_TR_NOOP("warnings, the ESC line, a failed run, missing keys")),
    COLOUR("colors", "dim", dim, SNX_TR_NOOP("the boxes' frames, a filled box, the grid and the title's shadow")),
    COLOUR("colors", "warning", warning, SNX_TR_NOOP("the abort question in the console layout")),
    COLOUR("colors", "added", added,
           SNX_TR_NOOP("a diff's added lines, as failure its removed ones; no screen uses it yet")),
    WORD("head", "style", head, heads, SNX_TR_NOOP("bar: the tabs and the title on a band of ink")),
    FLAG("head", "rule", head_rule, SNX_TR_NOOP("a line under the head")),
    NUMBER("head", "size", title_size, 3, SNX_TR_NOOP("the title's letters, times the text's, 1 to 3")),
    FLAG("head", "shadow", title_shadow, SNX_TR_NOOP("a shadow under the title's letters, in the dim color")),
    WORD("head", "picture", picture, pictures,
         SNX_TR_NOOP("the user at the head's right, their nickname and picture in a box: none, square or round")),
    TEXT("head", "menu", title[GUI_SCREEN_MENU], SNX_TR_NOOP("the menu's title")),
    TEXT("head", "games", title[GUI_SCREEN_GAMES], SNX_TR_NOOP("the games/ list's title")),
    TEXT("head", "keys", title[GUI_SCREEN_KEYS], SNX_TR_NOOP("the keys screen's title")),
    TEXT("head", "compiling", title[GUI_SCREEN_COMPILING], SNX_TR_NOOP("a run's title until the game loads")),
    TEXT("head", "loading", title[GUI_SCREEN_LOADING], SNX_TR_NOOP("a run's title once the runner's banner came")),
    TEXT("body", "marker", marker, SNX_TR_NOOP("before each item")),
    WORD("body", "focus", focus_bar, focuses, SNX_TR_NOOP("the focused box in the focus color, or on a band of it")),
    WORD("body", "panels", panels, panel_words, SNX_TR_NOOP("the boxes outlined or filled")),
    WORD("body", "text", text, flows, SNX_TR_NOOP("a text too long for its box over its rows, or on one that slides")),
    TEXT("body", "cursor", cursor, SNX_TR_NOOP("in the margin, beside the focused box")),
    WORD(
        "body", "options-dialog", dialog, dialogs,
        SNX_TR_NOOP("a question: default, a box in the middle; yorha, at the foot, its choices at its right above it")),
    FLAG("backdrop", "grid", grid, SNX_TR_NOOP("a faint grid on the page, in the dim color")),
    FLAG("tips", "box", tips_box, SNX_TR_NOOP("in a box with a bar at its left, or on the page")),
    FLAG("tips", "wrap", tips_wrap, SNX_TR_NOOP("over the foot's rows, or on one row that slides")),
    WORD("footer", "style", footer, styles, SNX_TR_NOOP("bar: on a band of ink; none: no Status box")),
    FLAG("footer", "rule", footer_rule, SNX_TR_NOOP("a line over the foot")),
    FLAG("footer", "box", footer_box, SNX_TR_NOOP("the keys in a box with a bar at its left, or on the page")),
    WORD("run", "layout", run, layouts, SNX_TR_NOOP("steps: the loading screen, in the colors of the keys below")),
    COLOUR("run", "page", run_colors.page, SNX_TR_NOOP("the loading screen's background")),
    COLOUR("run", "ink", run_colors.ink, SNX_TR_NOOP("its text, the cursor, the ring's turning bar")),
    COLOUR("run", "soft", run_colors.soft, SNX_TR_NOOP("older steps, the name, the inner ring")),
    COLOUR("run", "dim", run_colors.dim, SNX_TR_NOOP("the outer ring, the bar's empty segments")),
    COLOUR("run", "failure", run_colors.failure, SNX_TR_NOOP("a failed step, a failed run")),
    COLOUR("run", "warning", run_colors.warning, SNX_TR_NOOP("a run being aborted, the question")),
    FLAG("run", "texture", texture, SNX_TR_NOOP("faint lines over the page")),
    FLAG("run", "bar", bar, SNX_TR_NOOP("the progress bar and its percentage")),
    FLAG("run", "detailed", detailed, SNX_TR_NOOP("each step's other lines under it")),
    FLAG("run", "collapse", collapse, SNX_TR_NOOP("a done step's lines folded away")),
    WORD("run", "ring", ring, rings, SNX_TR_NOOP("the turning ring")),
    WORD("run", "progress", progress, progresses,
         SNX_TR_NOOP("a running step's progress: amount (1.2 / 4.0 GB), percent, time left")),
    WORD("prompt", "align", prompt.align, aligns, SNX_TR_NOOP("on its row")),
    TEXT("prompt", "open", prompt.open, SNX_TR_NOOP("before the line")),
    TEXT("prompt", "close", prompt.close, SNX_TR_NOOP("after it")),
    FLAG("prompt", "blink", prompt.blink, SNX_TR_NOOP("blinking at blink_ms")),
    FLAG("prompt", "keys", prompt.lines[GUI_PROMPT_KEYS], SNX_TR_NOOP("the keys screen's drop line")),
    FLAG("prompt", "done", prompt.lines[GUI_PROMPT_DONE], SNX_TR_NOOP("the ESC line after a run")),
    FLAG("prompt", "failed", prompt.lines[GUI_PROMPT_FAILED], SNX_TR_NOOP("the ESC line after a failed run")),
    FLAG("prompt", "aborting", prompt.lines[GUI_PROMPT_ABORTING], SNX_TR_NOOP("a run being aborted")),
    NUMBER("transitions", "fade_in_ms", fade_in_ms, 10000, SNX_TR_NOOP("what comes after a fade out, from black")),
    NUMBER("transitions", "fade_out_ms", fade_out_ms, 10000, SNX_TR_NOOP("the menu as a run starts, a run ESC leaves")),
    WORD("transitions", "line_in", line_in, entrances, SNX_TR_NOOP("how a step's line comes in")),
    NUMBER("transitions", "line_in_ms", line_in_ms, 10000, SNX_TR_NOOP("slide and fade's length")),
    NUMBER("transitions", "typing_ms", typing_ms, 10000,
           SNX_TR_NOOP("a character of the running line, any other line whole")),
    WORD("transitions", "typing", typing_sync, typings, SNX_TR_NOOP("sync: one line at a time")),
    NUMBER("transitions", "blink_ms", blink_ms, 10000,
           SNX_TR_NOOP("the cursor's, the prompt's and Install Keys' blink")),
    NUMBER("transitions", "scroll_ms", scroll_ms, 10000,
           SNX_TR_NOOP("a line too long for its row slides a column each")),
    COLOUR("splash", "page", splash_page, SNX_TR_NOOP("behind the drawing")),
    NUMBER("splash", "ms", splash_ms, 10000, SNX_TR_NOOP("how long; 0: none")),
    NUMBER("splash", "chance", splash_chance, 100,
           SNX_TR_NOOP("how many boots in 100 show this theme's splash; the others show the default's")),
    {NULL, NULL, GUI_KEY_TEXT, 0, 0, NULL, 0, NULL, 0, 0, 0, 0, NULL}};
static const char* const sections[][2] = {
    {"theme", "Who made it. The theme's id is its folder's name."},
    {"colors", "The menus' colours, #RRGGBB."},
    {"head", "The top: the menu, Games and the Settings as tabs, and a title per screen."},
    {"body", "The screens' boxes: a list, the box beside it about the focus, the Status box under that one."},
    {"backdrop", "The page behind the boxes."},
    {"tips", "What the focus is about, or the screen's message."},
    {"footer", "The foot: the screen's keys, the tips beside them; the Status box: resource use, the working folder."},
    {"run", "A run: the driver's output as it comes, or a loading screen in its own colours."},
    {"prompt", "A line that waits on the user: framed, placed and blinking when its key is true."},
    {"transitions", "Milliseconds, 0 to 10000; 0 turns the thing off."},
    {"splash", "The boot's: one of Wimsy's drawings on a page."}};

/* "quoted" keeps its spaces */
static char* unquote(char* s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = 0;
        return s + 1;
    }
    return s;
}
static int copy(char* out, size_t size, const char* value) {
    if (strlen(value) >= size) return 0;
    strcpy(out, value);
    return 1;
}
static int colour(GuiRgb* out, const char* value) {
    unsigned r, g, b;
    if (strlen(value) != 7 || value[0] != '#' || strspn(value + 1, "0123456789abcdefABCDEF") != 6) return 0;
    sscanf(value + 1, "%2x%2x%2x", &r, &g, &b);
    out->r = (uint8_t)r;
    out->g = (uint8_t)g;
    out->b = (uint8_t)b;
    return 1;
}
/* a whole number, 0 to 10000 */
static int integer(uint32_t* out, const char* value) {
    char* end;
    unsigned long n = strtoul(value, &end, 10);
    if (!*value || *end || n > 10000) return 0;
    *out = (uint32_t)n;
    return 1;
}
/* The number a setting's typed row takes: the whole of value digits, one '.', two decimals at most (the rest of them
   dropped, 2.223 as 2.22), the sign of its range first, and within lo to hi; trimmed into out. A value past either end
   is the closest in range (1.1 of 2 to 5 is 2, 7 is 5), and a whole row's 3.0 is 3 where 3.5 takes no whole. Its
   leading zeros are left out. 0 when value is no number, 1 as it is, 2 the closest in range. Letters are never part of
   it, an exponent's e included */
static int number(const char* value, int whole, double lo, double hi, char* out, size_t size) {
    size_t n = 0, digits = 0, fraction = 0;
    const char* dot = NULL;
    char* end;
    double d;
    int clamped = 0;
    while (*value == ' ' || *value == '\t') value++;
    if (!*value) { /* no pick: the row is the .ini's again */
        out[0] = 0;
        return 1;
    }
    if (*value == '-' && lo < 0) {
        if (n + 1 >= size) return 0;
        out[n++] = *value++;
    }
    for (; *value && *value != ' ' && *value != '\t'; value++) {
        if (*value == '.') {
            if (dot) return 0;
            dot = value;
        } else if (*value >= '0' && *value <= '9') {
            if (dot) {
                if (fraction == 2) continue; /* 2.223: the third decimal and those after it are dropped */
                ++fraction;
            } else
                ++digits;
        } else
            return 0;
        if (n + 1 >= size) return 0;
        out[n++] = *value;
    }
    out[n] = 0;
    while (*value == ' ' || *value == '\t') value++;
    if (*value || (!digits && !fraction)) return 0; /* " ." and "-" alone take no number */
    d = strtod(out, &end);
    if (end == out || *end || d != d) return 0;
    if (!whole && lo > 0 && d == 0) return 0; /* 0 is no scale where the range starts above it */
    if (d < lo) {
        d = lo;
        clamped = 1;
    } else if (d > hi) {
        d = hi;
        clamped = 1;
    }
    if (whole) { /* "3.0" as 3; a real fraction is no whole number, as the runtime's check of an int says */
        long long v = (long long)d;
        if ((double)v != d) return 0;
        snprintf(out, size, "%lld", v);
    } else /* a scale as it reads: 2.0 as 2, 01.60 as 1.6, .25 as 0.25 */
        snprintf(out, size, "%g", d);
    return clamped ? 2 : 1;
}
const char* gui_theme_section_about(const char* section) {
    size_t i;
    for (i = 0; i < sizeof sections / sizeof *sections; ++i)
        if (!strcmp(sections[i][0], section)) return sections[i][1];
    return "";
}
const GuiThemeKey* gui_theme_key(const char* section, const char* key) {
    const GuiThemeKey* k;
    for (k = gui_theme_keys; k->section; ++k)
        if (!strcmp(k->section, section) && !strcmp(k->key, key)) return k;
    return NULL;
}
int gui_theme_set(void* object, const GuiThemeKey* key, const char* value) {
    char* field = (char*)object + key->at;
    uint32_t n;
    int i;
    switch (key->type) {
        case GUI_KEY_TEXT: /* a line of the file, or the number a setting is */
            if (strchr(value, '\n')) return 0;
            if (!key->numeric) return copy(field, key->size, value);
            {
                char text[64];
                int took = number(value, key->whole, key->lo, key->hi, text, sizeof text);
                if (!took || !copy(field, key->size, text)) return 0;
                return took;
            }
        case GUI_KEY_COLOUR: return colour((GuiRgb*)field, value);
        case GUI_KEY_FLAG:
            if (strcmp(value, "true") && strcmp(value, "false")) return 0;
            *(int*)field = value[0] == 't';
            return 1;
        case GUI_KEY_NUMBER:
            if (!integer(&n, value)) return 0;
            *(uint32_t*)field = n < key->max ? n : key->max;
            return 1;
        case GUI_KEY_WORD:
            for (i = 0; key->words[i]; ++i)
                if (!strcmp(value, key->words[i])) {
                    *(int*)field = i;
                    return 1;
                }
            return 0;
        case GUI_KEY_ACTION: return 0; /* nothing to set */
    }
    return 0;
}
int gui_theme_check(const GuiThemeKey* key, const char* value) {
    char text[64];
    GuiRgb c;
    uint32_t n;
    int i;
    switch (key->type) {
        case GUI_KEY_TEXT:
            if (strchr(value, '\n')) return 0;
            if (!key->numeric) return strlen(value) < key->size;
            return number(value, key->whole, key->lo, key->hi, text, sizeof text);
        case GUI_KEY_COLOUR: return colour(&c, value);
        case GUI_KEY_FLAG: return !strcmp(value, "true") || !strcmp(value, "false");
        case GUI_KEY_NUMBER: return !integer(&n, value) ? 0 : n > key->max ? 2 : 1;
        case GUI_KEY_WORD:
            for (i = 0; key->words[i]; ++i)
                if (!strcmp(value, key->words[i])) return 1;
            return 0;
        case GUI_KEY_ACTION: return 0;
    }
    return 0;
}
void gui_theme_get(const void* object, const GuiThemeKey* key, char* out, size_t size) {
    const char* field = (const char*)object + key->at;
    const GuiRgb* c = (const GuiRgb*)field;
    int i, n;
    switch (key->type) {
        case GUI_KEY_TEXT: snprintf(out, size, "%s", field); return;
        case GUI_KEY_COLOUR: snprintf(out, size, "#%02X%02X%02X", c->r, c->g, c->b); return;
        case GUI_KEY_FLAG: snprintf(out, size, "%s", *(const int*)field ? "true" : "false"); return;
        case GUI_KEY_NUMBER: snprintf(out, size, "%lu", (unsigned long)*(const uint32_t*)field); return;
        case GUI_KEY_WORD:
            i = *(const int*)field;
            for (n = 0; key->words[n]; ++n) {}
            snprintf(out, size, "%s", key->words[i >= 0 && i < n ? i : 0]);
            return;
        case GUI_KEY_ACTION: snprintf(out, size, "%s", key->size ? field : ""); return;
    }
}
/* What a file has that the theme does not take as written, for the menu to say: each "[section] key = value", why and
   what the theme has instead, joined by "; " in said (size bytes; said NULL says nothing) */
typedef struct {
    char* said;
    size_t size;
    const char* from; /* the built-in the file starts from */
    int foreign;      /* an ErET file: its keys the GUI has no place for are its own, not mistakes */
} Said;
static void say(Said* to, const char* format, ...) {
    va_list ap;
    size_t n;
    if (!to->said || !to->size) return;
    n = strlen(to->said);
    if (n && n + 2 < to->size) {
        memcpy(to->said + n, "; ", 3);
        n += 2;
    }
    if (n + 1 >= to->size) return;
    va_start(ap, format);
    vsnprintf(to->said + n, to->size - n, format, ap);
    va_end(ap);
}
/* what k reads, for a value it cannot */
static void takes(const GuiThemeKey* k, char* out, size_t size) {
    size_t n = 0;
    int i, count = 0;
    switch (k->type) {
        case GUI_KEY_COLOUR: snprintf(out, size, "%s", snx_tr("a color as #RRGGBB")); return;
        case GUI_KEY_FLAG: snprintf(out, size, "%s", snx_tr("true or false")); return;
        case GUI_KEY_NUMBER: snprintf(out, size, snx_tr("a whole number from 0 to %lu"), (unsigned long)k->max); return;
        case GUI_KEY_TEXT:
            snprintf(out, size, snx_tr("one line of at most %lu characters"), (unsigned long)(k->size - 1));
            return;
        case GUI_KEY_WORD:
            while (k->words[count]) ++count;
            out[0] = 0;
            for (i = 0; i < count && n < size; ++i)
                n += (size_t)snprintf(out + n, size - n, "%s%s", i ? (i == count - 1 ? snx_tr(" or ") : ", ") : "",
                                      k->words[i]);
            return;
        case GUI_KEY_ACTION: out[0] = 0; return;
    }
}
/* value into k of t, the file's section and key named: one it cannot read said with the value t keeps, a number past
   k's most with the one it takes */
static void set(GuiTheme* t, const GuiThemeKey* k, const char* section, const char* key, const char* value, Said* to) {
    uint32_t n;
    char was[96], what[160];
    gui_theme_get(t, k, was, sizeof was);
    if (!gui_theme_set(t, k, value)) {
        takes(k, what, sizeof what);
        say(to, snx_tr("[%s] %s = %s is not %s: it keeps %s"), section, key, value, what, was);
    } else if (k->type == GUI_KEY_NUMBER && integer(&n, value) && n > k->max)
        say(to, snx_tr("[%s] %s = %s is more than %lu: it takes %lu"), section, key, value, (unsigned long)k->max,
            (unsigned long)k->max);
}
static const GuiTheme* layout_base(const char* layout);
/* run_colors: an ErET loading theme's [colors] are its loading screen's, here the run's */
static void apply(GuiTheme* t, const char* section, const char* key, const char* value, int run_colors, Said* to) {
    const GuiThemeKey* k;
    uint32_t number;
    if (!strcmp(section, "theme") && !strcmp(key, "format")) {
        if (integer(&number, value) && number)
            t->format = number;
        else
            say(to, snx_tr("[theme] format = %s is not a whole number from 1: it keeps %lu"), value,
                (unsigned long)t->format);
        return;
    }
    if (!strcmp(section, "theme") && !strcmp(key, "base")) { /* read before the rest */
        if (!gui_theme_builtin(value))
            say(to, snx_tr("[theme] base = %s is not a built-in theme (default or yorha): it starts from %s"), value,
                to->from);
        return;
    }
    if (!strcmp(section, "theme") && !strcmp(key, "layout")) { /* ErET's, read before the rest */
        if (!layout_base(value))
            say(to, snx_tr("[theme] layout = %s is not menu, loading, plain, debug or skip: it starts from %s"), value,
                to->from);
        return;
    }
    /* ErET's places: [colors] texture, and with its loading layout the run's colours; [header] ring_side in its
       words, none being ours; [loading] title, its boot word */
    if (!strcmp(section, "colors") && (run_colors || !strcmp(key, "texture"))) {
        k = gui_theme_key("run", key);
        if (k && (k->type == GUI_KEY_COLOUR || !strcmp(key, "texture")))
            set(t, k, section, key, value, to);
        else if (!to->foreign)
            say(to, snx_tr("[%s] %s = %s is not a key of a theme"), section, key, value);
        return;
    }
    if (!strcmp(section, "header") && !strcmp(key, "ring_side")) {
        if (strcmp(value, "none")) set(t, gui_theme_key("run", "ring"), section, key, value, to);
        return;
    }
    if (!strcmp(section, "loading") && !strcmp(key, "title")) {
        set(t, gui_theme_key("head", "loading"), section, key, value, to);
        return;
    }
    if (!strcmp(section, "body") && (!strcmp(key, "gap") || !strcmp(key, "scroll"))) { /* docs/DEPRECATIONS.md */
        say(to, snx_tr("[body] %s = %s does nothing: ui.txt lays out the lists now (delete it)"), key, value);
        return;
    }
    if ((!strcmp(section, "head") && !strcmp(key, "place")) || (!strcmp(section, "tips") && !strcmp(key, "place")) ||
        (!strcmp(section, "body") && !strcmp(key, "sizing"))) { /* docs/DEPRECATIONS.md */
        say(to, snx_tr("[%s] %s = %s does nothing: ui.txt lays out the screens now (delete it)"), section, key, value);
        return;
    }
    k = gui_theme_key(section, key);
    if (k)
        set(t, k, section, key, value, to);
    else if (!to->foreign)
        say(to, snx_tr("[%s] %s = %s is not a key of a theme"), section, key, value);
}
/* each key = value of text, with its section, to visit; false when out of memory */
/* ErET's layout as the built-in to start from, when no base names one */
static const GuiTheme* layout_base(const char* layout) {
    if (!strcmp(layout, "menu")) return gui_theme_builtin("yorha");
    if (!strcmp(layout, "loading")) return gui_theme_builtin("yorha");
    if (!strcmp(layout, "plain") || !strcmp(layout, "debug") || !strcmp(layout, "skip"))
        return gui_theme_builtin("default");
    return NULL;
}
static int visit(const char* text, size_t size, GuiTheme* theme, const GuiTheme** bases, int base_only, int run_colors,
                 Said* to) {
    SnxIni ini;
    char* key;
    char* value;
    char* copy_of = malloc(size + 1);
    if (!copy_of) return 0;
    memcpy(copy_of, text, size);
    copy_of[size] = 0;
    snx_ini_start(&ini, copy_of);
    while (snx_ini_next(&ini, &key, &value)) {
        const char* section = ini.section;
        value = unquote(value);
        if (base_only) {
            if (!strcmp(section, "theme") && !strcmp(key, "base") && gui_theme_builtin(value))
                bases[0] = gui_theme_builtin(value);
            else if (!strcmp(section, "theme") && !strcmp(key, "layout") && layout_base(value)) {
                bases[1] = layout_base(value);
                bases[2] = !strcmp(value, "loading") ? bases[1] : NULL;
            }
        } else
            apply(theme, section, key, value, run_colors, to);
    }
    free(copy_of);
    return 1;
}
/* the built-ins, read from their files the first time: each one over nothing, as its file sets every key */
static const GuiTheme* builtins_read(void) {
    size_t i;
    if (builtins[0].id[0]) return builtins;
    for (i = 0; i < SNX_GUI_THEME_COUNT; ++i) {
        GuiTheme* t = &builtins[i];
        memset(t, 0, sizeof *t);
        copy(t->id, sizeof t->id, snx_gui_theme_ids[i]);
        Said none = {NULL, 0, "", 0};
        if (!visit(snx_gui_theme_texts[i], strlen(snx_gui_theme_texts[i]), t, NULL, 0, 0, &none)) t->id[0] = 0;
    }
    return builtins;
}
const GuiTheme* gui_splash_theme(const GuiTheme* theme, int roll) {
    return roll < (int)theme->splash_chance ? theme : gui_theme_builtin("default");
}
const GuiTheme* gui_theme_builtin(const char* id) {
    size_t i;
    const GuiTheme* all = builtins_read();
    for (i = 0; id && i < SNX_GUI_THEME_COUNT; ++i)
        if (all[i].id[0] && !strcmp(all[i].id, id)) return &all[i];
    return NULL;
}

int gui_theme_parse(GuiTheme* theme, const char* id, const char* text, size_t size, char* said, size_t said_size) {
    const GuiTheme* bases[3] = {NULL, NULL, NULL}; /* the base, ErET's layout, set when that is loading */
    char from[sizeof theme->id] = "";
    Said to = {said, said_size, from, 0};
    if (said && said_size) said[0] = 0;
    if (!visit(text, size, theme, bases, 1, 0, &to)) return 0;
    *theme = bases[0] ? *bases[0] : bases[1] ? *bases[1] : builtins_read()[0];
    copy(from, sizeof from, theme->id);
    to.foreign = bases[1] != NULL;
    copy(theme->id, sizeof theme->id, id);
    copy(theme->name, sizeof theme->name, id);
    return visit(text, size, theme, bases, 0, !bases[0] && bases[2], &to);
}
/* the first dirs[i]/id/theme.ini that can be read, its path in *path (owned) */
static char* theme_file(const char* const* dirs, size_t count, const char* id, size_t* size, char** path) {
    size_t i;
    char* text = NULL;
    *path = NULL;
    for (i = 0; i < count && !text; ++i) {
        char* folder = dirs[i] ? snx_path_join(dirs[i], id) : NULL;
        free(*path);
        *path = folder ? snx_path_join(folder, "theme.ini") : NULL;
        text = *path ? (char*)snx_read_file(*path, size) : NULL;
        free(folder);
    }
    return text;
}
char* gui_theme_text(const char* const* dirs, size_t count, const char* id, size_t* size) {
    char* path;
    char* text = theme_file(dirs, count, id, size, &path);
    free(path);
    return text;
}
void gui_theme_load(GuiTheme* theme, const char* const* dirs, size_t count, const char* id, char* said,
                    size_t said_size) {
    size_t size = 0;
    char *path, *text = theme_file(dirs, count, id, &size, &path), problems[1024];
    const GuiTheme* builtin = gui_theme_builtin(id);
    const GuiTheme* shown = builtin ? builtin : builtins_read();
    if (said && said_size) said[0] = 0;
    if (text && gui_theme_parse(theme, id, text, size, problems, sizeof problems)) {
        if (problems[0] && said) snprintf(said, said_size, "%s: %s", path, problems);
    } else {
        *theme = *shown;
        if (said && text) /* read, but no memory to parse it */
            snprintf(said, said_size, snx_tr("%s could not be read (out of memory): it shows %s"), path, shown->id);
        else if (said && !builtin)
            snprintf(said, said_size, snx_tr("No themes/%s/theme.ini could be read: it shows %s"), id, shown->id);
    }
    free(text);
    free(path);
}

void gui_themes_free(GuiThemes* themes) {
    free(themes->ids);
    themes->ids = NULL;
    themes->count = 0;
}
static int listed(const GuiThemes* themes, const char* id) {
    size_t i;
    for (i = 0; i < themes->count; ++i)
        if (!strcmp(themes->ids[i], id)) return 1;
    return 0;
}
void gui_themes_scan(GuiThemes* themes, const char* const* dirs, size_t count) {
    size_t total, capacity, i, d;
    gui_themes_free(themes);
    total = SNX_GUI_THEME_COUNT;
    capacity = total;
    themes->ids = calloc(capacity, sizeof *themes->ids);
    if (!themes->ids) return;
    for (i = 0; i < total; ++i) memcpy(themes->ids[themes->count++], builtins_read()[i].id, sizeof *themes->ids);
    for (d = 0; d < count; ++d) {
        char** names = NULL;
        size_t found = 0;
        char (*grown)[sizeof *themes->ids];
        if (!dirs[d] || snx_list_dir(dirs[d], &names, &found)) continue; /* no such folder: nothing from it */
        grown = realloc(themes->ids, (capacity + found) * sizeof *themes->ids);
        if (!grown) {
            snx_free_strings(names, found);
            continue;
        }
        themes->ids = grown;
        capacity += found;
        for (i = 0; i < found; ++i) {
            char* folder = snx_path_join(dirs[d], names[i]);
            char* file = folder ? snx_path_join(folder, "theme.ini") : NULL;
            int usable = names[i][0] != '.' && !listed(themes, names[i]) && strlen(names[i]) < sizeof *themes->ids &&
                         file && snx_is_file(file);
            free(file);
            free(folder);
            if (usable) snprintf(themes->ids[themes->count++], sizeof *themes->ids, "%s", names[i]);
        }
        snx_free_strings(names, found);
    }
}

/* text that grows as it is written; failed once out of memory */
typedef struct {
    char* p;
    size_t n, room;
    int failed;
} Out;
static void put(Out* o, const char* format, ...) {
    va_list ap;
    int n;
    for (;;) {
        char* grown;
        if (o->failed) return;
        if (o->room) {
            va_start(ap, format);
            n = vsnprintf(o->p + o->n, o->room - o->n, format, ap);
            va_end(ap);
            if (n < 0) o->failed = 1;
            if (n < 0 || o->n + (size_t)n < o->room) {
                o->n += n < 0 ? 0 : (size_t)n;
                return;
            }
        }
        o->room = o->room ? o->room * 2 : 4096;
        grown = realloc(o->p, o->room);
        if (!grown) {
            free(o->p);
            o->p = NULL;
            o->failed = 1;
        }
        o->p = grown;
    }
}
/* quotes keep white space at either end, and a value that is itself quoted */
static void put_key(Out* o, const char* key, const char* value) {
    size_t n = strlen(value);
    int quoted = n && (value[0] == ' ' || value[0] == '\t' || value[n - 1] == ' ' || value[n - 1] == '\t' ||
                       value[n - 1] == '\r' || (n >= 2 && value[0] == '"' && value[n - 1] == '"'));
    put(o, quoted ? "%s = \"%s\"\n" : "%s = %s\n", key, value);
}
static const char* const eret_layouts[] = {"plain", "debug", "loading", "menu", "skip", NULL};
static int is_word(const char* const* words, const char* w) {
    for (; *words; ++words)
        if (!strcmp(*words, w)) return 1;
    return 0;
}

int gui_theme_file_read(GuiThemeFile* file, const char* text, size_t size) {
    Out others = {NULL, 0, 0, 0};
    SnxIni ini;
    char *key, *value, last[sizeof ini.section] = "";
    char* copy_of = malloc(size + 1);
    memset(file, 0, sizeof *file);
    if (!copy_of) return 0;
    memcpy(copy_of, text, size);
    copy_of[size] = 0;
    snx_ini_start(&ini, copy_of);
    while (snx_ini_next(&ini, &key, &value)) {
        value = unquote(value);
        if (!strcmp(ini.section, "theme") && !strcmp(key, "base"))
            copy(file->base, sizeof file->base, value);
        else if (!strcmp(ini.section, "theme") && !strcmp(key, "layout"))
            copy(file->layout, sizeof file->layout, value);
        else if (!strcmp(ini.section, "header") && !strcmp(key, "ring_side"))
            file->ring_side = 1;
        else if (!strcmp(ini.section, "loading") && !strcmp(key, "title"))
            file->loading_title = 1;
        else if (!strcmp(ini.section, "colors") && !strcmp(key, "texture"))
            file->colors_texture = 1;
        else if (ini.section[0] && key[0] && !gui_theme_key(ini.section, key) &&
                 (strcmp(ini.section, "theme") || strcmp(key, "format"))) {
            if (strcmp(last, ini.section)) put(&others, "[%s]\n", strcpy(last, ini.section));
            put_key(&others, key, value);
        }
    }
    free(copy_of);
    file->others = others.p;
    return !others.failed;
}
void gui_theme_file_free(GuiThemeFile* file) {
    free(file->others);
    file->others = NULL;
}
/* the [theme] layout a built-in's file names: ErET's drawing for a theme that starts from it */
static const char* builtin_layout(const char* id) {
    static GuiThemeFile files[SNX_GUI_THEME_COUNT];
    size_t i;
    for (i = 0; i < SNX_GUI_THEME_COUNT; ++i)
        if (!strcmp(snx_gui_theme_ids[i], id)) {
            if (!files[i].layout[0])
                gui_theme_file_read(&files[i], snx_gui_theme_texts[i], strlen(snx_gui_theme_texts[i]));
            return is_word(eret_layouts, files[i].layout) ? files[i].layout : "debug";
        }
    return "debug";
}
char* gui_theme_write(const GuiTheme* theme, const GuiThemeFile* file) {
    Out o = {NULL, 0, 0, 0};
    const char* base = file && gui_theme_builtin(file->base) ? file->base : "default";
    const char* layout = file && is_word(eret_layouts, file->layout) ? file->layout : builtin_layout(base);
    int loading = !strcmp(layout, "loading"); /* ErET's: no base, it starts from YoRHa, [colors] are the run's */
    char value[80];
    size_t s;
    put(&o,
        "; ScarletNX GUI theme, format %d: a folder of themes/ holding this file, the folder's name its id. A\n"
        "; folder named like a built-in replaces it. Keys left out, or that cannot be read, keep the base's value;\n"
        "; keys this GUI does not know are left for others (rEden's ErET reads the same file). A comment takes a\n"
        "; line of its own. docs/GUI.md describes every key. Made with the theme editor in ScarletNX's menu.\n",
        GUI_THEME_FORMAT);
    for (s = 0; s < sizeof sections / sizeof *sections; ++s) {
        const char* name = sections[s][0];
        int run_colors = loading && !strcmp(name, "colors");
        const GuiThemeKey* k;
        put(&o, "%s[%s]\n; %s\n", s ? "\n" : "", name,
            run_colors ? "The loading screen's colours (a run's), as [run] has them in other layouts."
                       : sections[s][1]);
        /* a newer format's file keeps its number: the keys of it this GUI does not know are kept as they are */
        if (!s)
            put(&o, "format = %lu\n",
                (unsigned long)(theme->format > GUI_THEME_FORMAT ? theme->format : GUI_THEME_FORMAT));
        for (k = gui_theme_keys; k->section; ++k) {
            if (strcmp(k->section, run_colors ? "run" : name)) continue;
            if (run_colors && k->type != GUI_KEY_COLOUR && strcmp(k->key, "texture")) continue;
            if (loading && !strcmp(name, "run") && k->type == GUI_KEY_COLOUR) continue;
            gui_theme_get(theme, k, value, sizeof value);
            put(&o, "; %s\n", k->about);
            put_key(&o, k->key, value);
        }
        if (s) continue;
        put(&o, "; ErET's drawing for it (plain | debug | loading | menu | skip); a base when no base names one\n");
        put_key(&o, "layout", layout);
        if (loading)
            put(&o, "; no base: ErET's loading layout starts from yorha, the menus in its colours\n");
        else {
            put(&o, "; the built-in it starts from: default | yorha\n");
            put_key(&o, "base", base);
        }
    }
    if (file && ((file->ring_side && theme->ring != GUI_RING_NONE) || file->loading_title ||
                 (file->colors_texture && !loading))) {
        put(&o, "\n; ErET's places for the ring, the boot word and the texture, the same as above\n");
        if (file->ring_side && theme->ring != GUI_RING_NONE) {
            put(&o, "[header]\n");
            put_key(&o, "ring_side", rings[theme->ring]);
        }
        if (file->loading_title) {
            put(&o, "[loading]\n");
            put_key(&o, "title", theme->title[GUI_SCREEN_LOADING]);
        }
        if (file->colors_texture && !loading) {
            put(&o, "[colors]\n");
            put_key(&o, "texture", theme->texture ? "true" : "false");
        }
    }
    if (file && file->others)
        put(&o, "\n; keys this GUI does not read, kept for the programs that do\n%s", file->others);
    if (o.failed) {
        free(o.p);
        return NULL;
    }
    return o.p;
}
static uint32_t rgb_word(GuiRgb c) { return (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b; }
typedef char run_progress_same[SNX_RUN_PROGRESS_AMOUNT == GUI_PROGRESS_AMOUNT &&
                                       SNX_RUN_PROGRESS_PERCENT == GUI_PROGRESS_PERCENT &&
                                       SNX_RUN_PROGRESS_TIME == GUI_PROGRESS_TIME
                                   ? 1
                                   : -1];
void gui_run_theme(const char* folder, SnxRunTheme* out) {
    char *config = snx_menu_dir(folder, SNX_USER_CONFIG), *data = snx_menu_dir(folder, SNX_USER_DATA);
    char* app = config ? snx_path_join(config, "app.ini") : NULL;
    size_t bytes = 0, i;
    char *text = app ? (char*)snx_read_file(app, &bytes) : NULL, *key, *value, id[64] = "yorha", picked[1024] = "";
    /* the running program's folder: the launcher's own when it started the run, as it runs itself with --game */
    char *self = snx_executable("scarletnx"), *program_dir = self ? snx_dirname(self) : NULL;
    char *beside = program_dir ? snx_path_join(program_dir, "themes") : NULL, *dirs[2] = {NULL, NULL};
    GuiTheme* t = malloc(sizeof *t);
    GuiThemes listed = {0};
    SnxIni ini;
    *out = (SnxRunTheme){0};
    /* as the launcher reads app.ini (src/gui/main.c app_read): its theme, its UI scale (ui_scale, text_size as an
       older version named it: a number it takes, else 0, its Auto), and the data folder Folders picked, a full path
       or none */
    if (text) {
        snx_ini_start(&ini, text);
        while (snx_ini_next(&ini, &key, &value))
            if (!strcmp(ini.section, "app") && !strcmp(key, "theme") && *value)
                snprintf(id, sizeof id, "%s", value);
            else if (!strcmp(ini.section, "app") && (!strcmp(key, "ui_scale") || !strcmp(key, "text_size"))) {
                char* end;
                long n = strtol(value, &end, 10);
                out->text_size = *value && !*end && n >= 1 && n <= GUI_TEXT_MAX ? (int)n : 0;
            } else if (!strcmp(ini.section, "folders") && !strcmp(key, "data") && snx_path_is_full(value))
                snprintf(picked, sizeof picked, "%s", value);
    }
    /* the launcher's two folders of themes (themes_dirs): its data folder's, then those shipped beside it (main.c
       shipped_themes), which an installed one keeps in ../share/scarletnx/themes. A game's copy of the runtime
       started on its own has none beside it, and finds the data folder's and the built-ins */
    dirs[0] = picked[0] ? snx_path_join(picked, "themes") : data ? snx_path_join(data, "themes") : NULL;
    if (beside && !snx_is_dir(beside)) {
        free(beside);
        beside = snx_path_join(program_dir, "../share/scarletnx/themes");
    }
    dirs[1] = beside;
    /* a theme it lists, as the launcher takes one only then (an id is a folder's name, never a path); else YoRHa,
       the launcher's own until another is picked */
    gui_themes_scan(&listed, (const char* const*)dirs, 2);
    for (i = 0; i < listed.count && strcmp(listed.ids[i], id); ++i) {}
    if (i == listed.count) snprintf(id, sizeof id, "%s", "yorha");
    if (t) {
        gui_theme_load(t, (const char* const*)dirs, 2, id, NULL, 0); /* its problems are the launcher's to say */
        out->page = rgb_word(t->run_colors.page);
        out->ink = rgb_word(t->run_colors.ink);
        out->soft = rgb_word(t->run_colors.soft);
        out->dim = rgb_word(t->run_colors.dim);
        snprintf(out->name, sizeof out->name, "%s", t->name);
        snprintf(out->title, sizeof out->title, "%s", t->title[GUI_SCREEN_LOADING]);
        out->steps = t->run == GUI_RUN_STEPS;
        out->texture = t->texture;
        out->progress = t->progress;
    }
    gui_themes_free(&listed);
    free(t);
    free(text);
    free(dirs[0]);
    free(dirs[1]);
    free(program_dir);
    free(self);
    free(app);
    free(data);
    free(config);
}
