/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/model.h"
#include "common/utilities/common.h"
#include "common/utilities/font.h"
#include "common/utilities/setup.h"
#include "gui/i18n.h"
#include "gui/latin.h"
#include "gui/wide.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the item before or after one, round the menu */
static int step_item(int item, int step) { return (item + GUI_ITEM_COUNT + step) % GUI_ITEM_COUNT; }
GuiMenuDo gui_menu_key(GuiMenu* menu, GuiKey key) {
    /* the browser owns its keys, and the queue its own */
    if (menu->running || menu->view == GUI_LOAD_GAMES || menu->view == GUI_LOAD_RECOMPILE) return GUI_MENU_STAY;
    if (menu->view != GUI_LOAD_NONE && key == GUI_CANCEL) {
        menu->view = GUI_LOAD_NONE;
        menu->selected = menu->active;
        return GUI_MENU_STAY;
    }
    if (menu->view == GUI_LOAD_KEYS) return GUI_MENU_STAY;
    switch (key) {
        case GUI_UP:
        case GUI_LEFT: menu->selected = step_item(menu->selected, -1); break;
        case GUI_DOWN:
        case GUI_RIGHT: menu->selected = step_item(menu->selected, 1); break;
        case GUI_SELECT:
            menu->active = menu->selected;
            menu->activated = true;
            menu->view = GUI_LOAD_NONE;
            switch (menu->selected) {
                case GUI_SWITCH:
                case GUI_LINUX:
                case GUI_3DS:
                case GUI_VITA: menu->view = GUI_LOAD_DROP; break;
                case GUI_GBA: menu->view = GUI_LOAD_DROP; break;
                case GUI_KEYS: menu->view = GUI_LOAD_KEYS; break;
                case GUI_SETTINGS: return GUI_MENU_SETTINGS;
                case GUI_RECOMMENDED: return GUI_MENU_RECOMMENDED;
                case GUI_LOG: return GUI_MENU_LOG;
                case GUI_FOLDERS: return GUI_MENU_FOLDERS;
                case GUI_EXIT: menu->quit = true; break;
                default: break;
            }
            break;
        default: break;
    }
    return GUI_MENU_STAY;
}
void gui_confirm_open(GuiConfirm* confirm, GuiAsk ask, bool twice) {
    confirm->shown = true;
    confirm->ask = ask;
    confirm->selected = GUI_CONFIRM_KEEP;
    confirm->twice = twice;
    confirm->armed = false;
}
GuiChoiceDo gui_choice_key(GuiChoice* choice, GuiKey key) {
    switch (key) {
        case GUI_UP:
        case GUI_LEFT:
            if (choice->count > 0) choice->focus = (choice->focus + choice->count - 1) % choice->count;
            return GUI_CHOICE_STAY;
        case GUI_DOWN:
        case GUI_RIGHT:
            if (choice->count > 0) choice->focus = (choice->focus + 1) % choice->count;
            return GUI_CHOICE_STAY;
        case GUI_SELECT: return choice->count > 0 ? GUI_CHOICE_TAKE : GUI_CHOICE_STAY;
        case GUI_CANCEL: return GUI_CHOICE_BACK;
        default: return GUI_CHOICE_STAY;
    }
}
GuiAppearanceDo gui_appearance_key(int* row, GuiKey key, int* step) {
    *step = 0;
    switch (key) {
        case GUI_UP: *row = (*row + GUI_APPEARANCE_ROWS - 1) % GUI_APPEARANCE_ROWS; return GUI_APPEARANCE_STAY;
        case GUI_DOWN: *row = (*row + 1) % GUI_APPEARANCE_ROWS; return GUI_APPEARANCE_STAY;
        case GUI_LEFT:
        case GUI_RIGHT:
            if (*row == GUI_APPEARANCE_NEXT) return GUI_APPEARANCE_STAY;
            *step = key == GUI_LEFT ? -1 : 1;
            return GUI_APPEARANCE_CHANGE;
        case GUI_SELECT:
            if (*row == GUI_APPEARANCE_TEXT) return GUI_APPEARANCE_SIZES;
            if (*row == GUI_APPEARANCE_NEXT) return GUI_APPEARANCE_ON;
            *step = 1;
            return GUI_APPEARANCE_CHANGE;
        case GUI_CANCEL: return GUI_APPEARANCE_BACK;
        default: return GUI_APPEARANCE_STAY;
    }
}
bool gui_confirm_key(GuiConfirm* confirm, GuiKey key) {
    GuiChoice two;
    if (!confirm->shown) return false;
    two.count = 2;
    two.focus = confirm->selected;
    switch (gui_choice_key(&two, key)) {
        case GUI_CHOICE_STAY: gui_confirm_focus(confirm, two.focus); return false;
        case GUI_CHOICE_TAKE:
            if (confirm->twice && confirm->selected == GUI_CONFIRM_ABORT && !confirm->armed) {
                confirm->armed = true;
                return false;
            }
            confirm->shown = false;
            return confirm->selected == GUI_CONFIRM_ABORT;
        default: confirm->shown = false; return false;
    }
}
void gui_confirm_focus(GuiConfirm* confirm, int choice) {
    if (choice != confirm->selected) confirm->armed = false;
    confirm->selected = choice;
}
void gui_size_text(char* out, size_t size, uint64_t bytes) {
    double mb = (double)bytes / 1048576;
    uint64_t kb = (bytes + 1023) / 1024;
    if (bytes && kb < 1024)
        snprintf(out, size, "%u KB", (unsigned)kb);
    else if (mb < 1024)
        snprintf(out, size, "%.0f MB", mb);
    else
        snprintf(out, size, "%.1f GB", mb / 1024);
}
bool gui_room(uint64_t dump, bool copy, uint64_t work_free, uint64_t games_free, bool shared, uint64_t* need,
              uint64_t* left) {
    *need = copy && shared ? 2 * dump : dump;
    *left = work_free;
    if (*need > work_free) return false;
    if (copy && !shared && dump > games_free) {
        *left = games_free;
        return false;
    }
    return true;
}
void gui_usage_text(char* out, size_t size, bool available, uint64_t memory_bytes, double cpu_percent, int cores) {
    double n = cores > 0 ? cores : 1, used = cpu_percent / 100;
    char ram[32];
    if (!available) {
        snprintf(out, size, "%s", snx_tr("RAM and CPU: unavailable"));
        return;
    }
    used = used < 0 ? 0 : used > n ? n : used; /* a sample taken over a little more than its interval */
    gui_size_text(ram, sizeof ram, memory_bytes);
    snprintf(out, size, snx_tr("RAM %s  CPU %.0f%% (%.1f cores)"), ram, used / n * 100, used);
}
const char* gui_setting_description(const GuiMenu* menu) {
    switch (menu->selected) {
        case GUI_SETTINGS: return snx_tr("The game's .ini settings, the launcher's for the next runs, and its theme");
        case GUI_RECOMMENDED:
            return snx_tr("Tests this computer in a few seconds, then proposes the settings that suit it; nothing "
                          "changes until Apply");
        case GUI_LOG:
            return snx_tr(
                "Saves the logs as one zip or as scarletnx.log in a folder it opens to share from, or opens their folder");
        case GUI_FOLDERS:
            return snx_tr(
                "Where the app keeps each kind of its files, and the libraries: each folder opened or changed");
        default: return "";
    }
}
#define GUI_SLIDE_WAIT 8 /* steps a sliding line rests at each end */
int gui_slide(int over, uint64_t ms, uint32_t step_ms) {
    uint64_t t;
    if (over <= 0 || !step_ms) return 0;
    t = ms / step_ms % (uint64_t)(2 * (over + GUI_SLIDE_WAIT));
    if (t < GUI_SLIDE_WAIT) return 0;
    if (t < (uint64_t)(GUI_SLIDE_WAIT + over)) return (int)(t - GUI_SLIDE_WAIT);
    if (t < (uint64_t)(2 * GUI_SLIDE_WAIT + over)) return over;
    return over - (int)(t - 2 * GUI_SLIDE_WAIT - over);
}
size_t gui_wrap(const char* s, int width) {
    const char* p = s;
    size_t slash = 0, space = 0;
    int cols = 0;
    while (*p) {
        const char* at = p;
        cols += gui_glyph_cols(gui_menu_glyph(gui_utf8_step(&p)));
        if (cols > (width < 1 ? 1 : width) && at > s) return slash ? slash : space ? space : (size_t)(at - s);
        if (p[-1] == '/' || p[-1] == '\\') slash = (size_t)(p - s);
        if (p[-1] == ' ') space = (size_t)(p - s);
    }
    return (size_t)(p - s);
}
size_t gui_browser_pages(const GuiBrowser* browser) {
    if (!browser->count || !browser->page_size) return 1;
    return 1 + (browser->count - 1) / browser->page_size;
}
size_t gui_browser_items(const GuiBrowser* browser) {
    if (!browser->page_size || browser->page >= gui_browser_pages(browser)) return 0;
    size_t remaining = browser->count - browser->page * browser->page_size;
    return remaining < browser->page_size ? remaining : browser->page_size;
}
/* the focus on the last row there is, when it is past it */
static void browser_clamp(GuiBrowser* browser) {
    size_t rows = browser->head + gui_browser_items(browser);
    if (browser->selected >= rows) browser->selected = rows ? rows - 1 : 0;
}
void gui_browser_resize(GuiBrowser* browser, size_t page_size) {
    int on_game = browser->selected >= browser->head;
    if (!page_size) page_size = 1;
    /* Preserve the selected file across a resize, not merely its row number. */
    size_t anchor = browser->page * browser->page_size;
    if (on_game) anchor += browser->selected - browser->head;
    browser->page_size = page_size;
    browser->page = anchor / page_size;
    size_t pages = gui_browser_pages(browser);
    if (browser->page >= pages) browser->page = pages - 1;
    if (on_game) browser->selected = browser->head + anchor % page_size;
    browser_clamp(browser);
}
/* whether row is an item skip leaves out */
static bool skipped(const GuiBrowser* browser, size_t row, bool (*skip)(size_t, const void*), const void* user) {
    return skip && row >= browser->head && row < browser->head + gui_browser_items(browser) &&
           skip(browser->page * browser->page_size + row - browser->head, user);
}
GuiBrowserAction gui_browser_key(GuiBrowser* browser, GuiKey key) {
    return gui_browser_key_skip(browser, key, NULL, NULL);
}
GuiBrowserAction gui_browser_key_skip(GuiBrowser* browser, GuiKey key, bool (*skip)(size_t item, const void* user),
                                      const void* user) {
    size_t rows = browser->head + gui_browser_items(browser), tries, r;
    switch (key) {
        case GUI_CANCEL: return GUI_BROWSER_BACK;
        case GUI_SELECT:
            if (browser->selected < browser->head) return GUI_BROWSER_HEAD;
            if (browser->selected >= rows || skipped(browser, browser->selected, skip, user)) return GUI_BROWSER_NONE;
            browser->active = browser->page * browser->page_size + browser->selected - browser->head;
            browser->activated = true;
            return GUI_BROWSER_OPEN;
        case GUI_UP: /* past a page's first item to the one before's last; from the head round to the list's last */
            for (tries = 0; tries <= browser->head + browser->count; ++tries) {
                if (browser->selected == browser->head && browser->page) {
                    --browser->page;
                    browser->selected = browser->head + gui_browser_items(browser) - 1;
                } else if (browser->selected)
                    --browser->selected;
                else {
                    browser->page = gui_browser_pages(browser) - 1;
                    rows = browser->head + gui_browser_items(browser);
                    browser->selected = rows ? rows - 1 : 0;
                }
                if (!skipped(browser, browser->selected, skip, user)) break;
            }
            break;
        case GUI_DOWN: /* past a page's last item to the next one's first; from the list's last round to the head */
            for (tries = 0; tries <= browser->head + browser->count; ++tries) {
                rows = browser->head + gui_browser_items(browser);
                if (browser->selected + 1 < rows)
                    ++browser->selected;
                else if (browser->page + 1 < gui_browser_pages(browser)) {
                    ++browser->page;
                    browser->selected = browser->head;
                } else {
                    browser->page = 0;
                    browser->selected = 0;
                }
                if (!skipped(browser, browser->selected, skip, user)) break;
            }
            break;
        case GUI_LEFT:
        case GUI_PAGE_UP:
            if (browser->page) --browser->page;
            break;
        case GUI_RIGHT:
        case GUI_PAGE_DOWN:
            if (browser->page + 1 < gui_browser_pages(browser)) ++browser->page;
            break;
        default: break;
    }
    browser_clamp(browser);
    if (skipped(browser, browser->selected, skip, user)) { /* a page whose focus fell on a heading */
        rows = browser->head + gui_browser_items(browser);
        for (r = browser->selected; r + 1 < rows && skipped(browser, r, skip, user); ++r) {}
        if (skipped(browser, r, skip, user))
            for (r = browser->selected; r > 0 && skipped(browser, r, skip, user); --r) {}
        browser->selected = r;
    }
    return GUI_BROWSER_NONE;
}
static int clamp(int n, int maximum) { return n < 1 ? 1 : n > maximum ? maximum : n; }
void gui_console_init(GuiConsole* console, int cols, int rows) {
    memset(console, 0, sizeof(*console));
    memset(console->cells, ' ', sizeof(console->cells));
    console->cols = clamp(cols, GUI_MAX_COLS);
    console->rows = clamp(rows, GUI_MAX_ROWS);
}
void gui_console_resize(GuiConsole* console, int cols, int rows) {
    cols = clamp(cols, GUI_MAX_COLS);
    rows = clamp(rows, GUI_MAX_ROWS);
    /* Only the visible screen is retained. Resizing is not a scrollback buffer. */
    if (console->row >= rows) {
        int drop = console->row - rows + 1;
        memmove(console->cells, console->cells[drop], (size_t)rows * GUI_MAX_COLS);
        memset(console->cells[rows], ' ', (size_t)(GUI_MAX_ROWS - rows) * GUI_MAX_COLS);
        console->row = rows - 1;
    }
    if (cols < console->cols)
        for (int i = 0; i < GUI_MAX_ROWS; ++i) memset(console->cells[i] + cols, ' ', GUI_MAX_COLS - (size_t)cols);
    console->cols = cols;
    console->rows = rows;
    if (console->col > cols) console->col = cols;
}
static void newline(GuiConsole* console) {
    console->col = 0;
    if (++console->row < console->rows) return;
    memmove(console->cells, console->cells[1], (size_t)(console->rows - 1) * GUI_MAX_COLS);
    memset(console->cells[console->rows - 1], ' ', GUI_MAX_COLS);
    console->row = console->rows - 1;
}
static void put_glyph(GuiConsole* console, unsigned char glyph) {
    if (console->col >= console->cols) newline(console);
    console->cells[console->row][console->col++] = glyph;
}
static void feed_byte(GuiConsole* console, unsigned char c) {
    if (console->escape) {
        if (console->escape == 1)
            console->escape = c == '[' ? 2 : c == ']' ? 3 : 0;
        else if (console->escape == 2 && c >= 0x40 && c <= 0x7e)
            console->escape = 0;
        else if (console->escape == 3 && c == 7)
            console->escape = 0;
        else if (console->escape == 3 && c == 27)
            console->escape = 4;
        else if (console->escape == 4)
            console->escape = c == '\\' ? 0 : 3;
        return;
    }
    if (console->utf8_left) {
        if ((c & 0xc0) == 0x80) {
            console->codepoint = (console->codepoint << 6) | (c & 63);
            if (!--console->utf8_left) {
                uint32_t cp = console->codepoint;
                put_glyph(console, cp < console->utf8_min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)
                                       ? '?'
                                       : gui_glyph(cp));
            }
            return;
        }
        console->utf8_left = 0;
        put_glyph(console, '?');
    }
    if (c == 27)
        console->escape = 1;
    else if (c == '\n')
        newline(console);
    else if (c == '\r')
        console->col = 0;
    else if (c == '\b') {
        if (console->col) --console->col;
    } else if (c == '\t') {
        int spaces = 8 - (console->col % 8);
        while (spaces--) put_glyph(console, ' ');
    } else if (c >= 32 && c < 127)
        put_glyph(console, c);
    else if (c >= 0xc2 && c <= 0xf4) {
        console->utf8_left = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        console->utf8_min = c < 0xe0 ? 0x80 : c < 0xf0 ? 0x800 : 0x10000;
        console->codepoint = c & (console->utf8_left == 1 ? 31 : console->utf8_left == 2 ? 15 : 7);
    } else if (c >= 128)
        put_glyph(console, '?');
}
void gui_console_feed(GuiConsole* console, const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) feed_byte(console, (unsigned char)data[i]);
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* size bytes of at, percent-decoded when uri; NULL when a % is not two hex digits, or makes a NUL */
static char* decoded(const char* at, size_t size, bool uri) {
    const char* end = at + size;
    size_t n = 0;
    char* out = malloc(size + 1);
    while (out && at < end) {
        unsigned char c = (unsigned char)*at++;
        if (uri && c == '%') {
            if (end - at < 2 || hex(at[0]) < 0 || hex(at[1]) < 0 ||
                !(c = (unsigned char)(hex(at[0]) << 4 | hex(at[1])))) {
                free(out);
                return NULL;
            }
            at += 2;
        }
        out[n++] = (char)c;
    }
    if (out) out[n] = 0;
    return out;
}
char* gui_drop_path(const char* text, bool text_drop) {
    if (!text) return NULL;
    const char* end = text + strlen(text);
    bool uri = false;
    if (text_drop) {
        while (text < end && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')) ++text;
        while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
        if (end - text >= 2 && ((*text == '"' && end[-1] == '"') || (*text == '\'' && end[-1] == '\''))) {
            ++text;
            --end;
        }
        if (end - text >= 7 && !memcmp(text, "file://", 7)) {
            text += 7;
            if (end - text >= 10 && !memcmp(text, "localhost/", 10)) text += 9;
            if (text == end || *text != '/') return NULL; /* no remote URI authorities */
            uri = true;
        }
    }
    if (text == end) return NULL;
    char* result = decoded(text, (size_t)(end - text), uri);
    if (result && text_drop && strpbrk(result, "\r\n")) {
        free(result);
        return NULL;
    }
#ifdef _WIN32
    if (result && uri && result[0] == '/' && result[1] && result[2] == ':') memmove(result, result + 1, strlen(result));
#endif
    return result;
}
char* gui_document_path(const char* uri) {
    static const char storage[] = "content://com.android.externalstorage.documents/document/";
    static const char tree[] = "content://com.android.externalstorage.documents/tree/"; /* a folder */
    static const char downloads[] = "content://com.android.providers.downloads.documents/document/";
    int download = !strncmp(uri, downloads, sizeof downloads - 1), folder = !strncmp(uri, tree, sizeof tree - 1);
    const char* id_text;
    char *id, *colon, *path = NULL;
    size_t n;
    if (!download && !folder && strncmp(uri, storage, sizeof storage - 1)) /* other systems' pickers: a path, file:// */
        return !strstr(uri, "://")           ? gui_drop_path(uri, false)
               : !strncmp(uri, "file://", 7) ? gui_drop_path(uri, true)
                                             : NULL;
    id_text = uri + (download ? sizeof downloads : folder ? sizeof tree : sizeof storage) - 1;
    id = decoded(id_text, strlen(id_text), true);
    colon = id ? strchr(id, ':') : NULL;
    if (colon && download && !strncmp(id, "raw:/", 5)) { /* a download by path; "msf:12" is by number */
        path = malloc(strlen(colon + 1) + 1);
        if (path) strcpy(path, colon + 1);
    } else if (colon && !download) {
        /* "primary:Download/x.xci" is the device's own storage (its first user's), "1234-5678:x.xci" a card */
        *colon = 0;
        n = strlen(id) + strlen(colon + 1) + 32;
        path = malloc(n);
        if (path) snprintf(path, n, "/storage/%s/%s", strcmp(id, "primary") ? id : "emulated/0", colon + 1);
    }
    free(id);
    return path;
}
int gui_keys_text(const char* data, size_t size) { return snx_keys_text(data, size); }
