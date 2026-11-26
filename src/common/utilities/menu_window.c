/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/menu_window.h"
#include "common/utilities/console_settings.h"
#include "common/utilities/game_log.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/menu_sdl.h"
#include "common/utilities/overlay.h"
#include "common/utilities/state_header.h"
#include "common/utilities/vpad.h"
#include "common/utilities/vpad_sdl.h"
#include "snx_settings.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    SnxSystem console;
    char *prefix, *name, *picks; /* the game's folder with its separator, its name, its picks' file */
    int quit, ask;               /* Exit taken; a window's action asked (a state's, the capture), -1 none */
    int relist;                  /* the slots read again this many draws from now: after a save or a delete */
} MenuWindow;
static MenuWindow g_menu_window = {.ask = -1};

static SDL_Window* window(void) { return snx_vpad_window_sdl(); }
static void size(int* w, int* h) { SDL_GetWindowSizeInPixels(window(), w, h); }
static void menu_open(int open) {
    if (overlay_shown() != open) overlay_toggle();
    vpad_release();
    overlay_pointers_reset();
}
static void act(int action) {
    if (action == OVERLAY_EXIT) g_menu_window.quit = 1;
    if (action == OVERLAY_CLOSE) menu_open(0);
    if (action == OVERLAY_SAVE || action == OVERLAY_LOAD) { /* the window's loop does it, between two frames */
        snx_vpad_window_slot(overlay_slot());
        g_menu_window.ask = action == OVERLAY_SAVE ? HOTKEY_SAVE_STATE : HOTKEY_LOAD_STATE;
        if (action == OVERLAY_LOAD) menu_open(0);
        g_menu_window.relist = 2;
    }
    if (action == OVERLAY_DELETE) { /* a file of the game's folder: gone at once */
        char name[16], path[SNX_PATH_SIZE];
        snx_state_slot_name(name, sizeof name, overlay_slot());
        snprintf(path, sizeof path, "%sstates/%s.%s" SNX_STATE_EXT, g_menu_window.prefix, g_menu_window.name, name);
        if (remove(path)) snx_vpad_window_log(1, "the state could not be deleted");
        g_menu_window.relist = 1;
    }
    if (action == OVERLAY_EDIT) {
        menu_open(0);
        vpad_edit(1);
    }
    if (action == OVERLAY_CAPTURE) { /* the frame after the menu's: the game's own */
        menu_open(0);
        g_menu_window.ask = HOTKEY_CAPTURE;
    }
}
static const SnxMenuWindow WINDOW = {
    .window = window, .size = size, .safe = snx_vpad_window_safe, .open = menu_open, .act = act};

/* the settings its console reads, and one changed: applied now, kept in the game's picks under its section, as the
   launcher's Settings of the game keep it */
static int reads(const char* key) { return snx_console_opt(g_menu_window.console, key) != NULL; }
static void set(const char* key, const char* value) {
    if (!vpad_ini(key, value)) {
        const IniOpt* o = snx_console_opt(g_menu_window.console, key);
        if (o) ini_read(o, value);
    }
    if (g_menu_window.picks && snx_ini_record_in(g_menu_window.picks, "Game", key, value))
        snx_vpad_window_log(1, "the game's picks could not keep the setting");
}
static const IniOpt* row(const char* key) { return snx_console_opt(g_menu_window.console, key); }
static const SnxMenuHost HOST = {.setting_shown = reads, .row = row, .set = set};

/* the slots the menu lists, read from their files' headers and thumbnails, as the Switch's are */
static void slots_list(void) {
    OverlaySlot list[OVERLAY_SLOTS_MAX] = {0};
    int n = g_settings.save_state_slots + 1;
    if (n > OVERLAY_SLOTS_MAX) n = OVERLAY_SLOTS_MAX;
    for (int i = 0; i < n; i++) {
        char name[16], path[SNX_PATH_SIZE], why[256];
        SnxStateHeader h;
        uint8_t* thumb = NULL;
        snx_state_slot_name(name, sizeof name, i);
        snprintf(path, sizeof path, "%sstates/%s.%s" SNX_STATE_EXT, g_menu_window.prefix, g_menu_window.name, name);
        if (snx_state_peek(path, &h, &thumb, why, sizeof why)) continue;
        snx_state_slot_when(list[i].when, sizeof list[i].when, &h);
        if (thumb && h.thumb_w == SNX_UI_IMAGE_W && h.thumb_h == SNX_UI_IMAGE_H)
            list[i].thumb = thumb;
        else
            free(thumb);
    }
    ui_lock();
    overlay_slots_set(list, n);
    ui_unlock();
}
static int event(const SDL_Event* e, int action, int* out) {
    ui_lock();
    int took = snx_menu_event(&WINDOW, e, action);
    ui_unlock();
    if (g_menu_window.quit) return 2;
    if (g_menu_window.ask >= 0) {
        *out = g_menu_window.ask;
        g_menu_window.ask = -1;
        return 3;
    }
    return took;
}
static int draw(SnxUiList* l, int w, int h) {
    if (overlay_states_stale() || (g_menu_window.relist && !--g_menu_window.relist)) slots_list();
    int before = l->n, full = snx_game_log_full_take();
    ui_lock();
    if (full) { /* the game log's first cut (game_log.c), said over the game once, as the Switch's */
        char said[96];
        snprintf(said, sizeof said, "Log full at %d MiB: older lines cut (Debug > Log size limit)", full);
        overlay_setting_toast("log_max", said, SDL_GetTicksNS());
    }
    overlay_draw(l, w, h, SDL_GetTicksNS());
    ui_unlock();
    return l->n > before;
}
void snx_menu_window_open(SnxSystem console, const char* folder, const char* name) {
    snx_menu_window_close();
    g_menu_window.console = console;
    g_menu_window.prefix = snx_format("%s/", folder ? folder : ".");
    g_menu_window.name = snx_strdup(name ? name : "game");
    char* ini = folder && name ? snx_game_ini(folder, name) : NULL;
    g_menu_window.picks = ini ? snx_game_picks(ini) : NULL;
    free(ini);
    overlay_set_host(&HOST);
    snx_vpad_window_menu(event, draw);
}
void snx_menu_window_close(void) {
    snx_vpad_window_menu(NULL, NULL);
    overlay_set_host(NULL);
    free(g_menu_window.prefix), free(g_menu_window.name), free(g_menu_window.picks);
    g_menu_window = (MenuWindow){.ask = -1};
}
