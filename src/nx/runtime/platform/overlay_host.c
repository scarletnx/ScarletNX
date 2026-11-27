/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the Switch's runtime gives the game's menu (overlay.h's SnxMenuHost): every part of it, from the renderer, the
   input recording, the TV mode, the touch screen, the gamepads, the screens before the game, the atlas's pictures, the
   controller applet, the debug HUD and Tracy */
#include "nx/runtime/platform/overlay_host.h"
#include "common/utilities/overlay.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/ctrl_applet.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/platform/unattended.h"
#include "nx/runtime/options.h"
#include "nx/gpu/renderer.h"
#include <string.h>

static int capture(int ask) { return g_renderer->capture(ask); }
/* a setting's row among the Switch's tables: the menu's own rows' (the backend's, the gamepads') and the sections' */
static const IniOpt* row(const char* key) {
    static const IniOpt* const tables[] = {g_display_ini, g_audio_ini, g_backend_ini, g_gamepad_ini,
                                           g_gamepad_players_ini};
    for (size_t i = 0; i < sizeof tables / sizeof *tables; ++i) {
        const IniOpt* o = ini_find(tables[i], key);
        if (o) return o;
    }
    for (int s = 0; s < g_ini_sections_count; ++s)
        for (int t = 0; t < 3 && g_ini_sections[s].table[t]; ++t) {
            const IniOpt* o = ini_find(g_ini_sections[s].table[t], key);
            if (o) return o;
        }
    return NULL;
}
static int pad_map(int player, SnxPadMap* own) {
    if (!player) {
        *own = gamepad_config()->own;
        return 1;
    }
    const SnxPlayerConfig* p = gamepad_player(player - 1);
    if (!p) return -1;
    if (!p->has_map) return 0;
    *own = p->own;
    return 1;
}
static void pad_map_set(int player, const SnxPadMap* own) {
    if (!player) {
        gamepad_config()->own = *own;
        gamepad_store_map();
        return;
    }
    SnxPlayerConfig* p = gamepad_player(player - 1);
    p->own = *own;
    p->has_map = 1;
    gamepad_store_player_map(player - 1);
}
#if defined(SNX_TRACY)
/* the menu's zones, as they were named where they are timed */
static const struct ___tracy_source_location_data ZONES[] = {
    [OVERLAY_ZONE_DRAW] = {"plat overlay draw", "overlay_draw", "overlay.c", 0, 0},
    [OVERLAY_ZONE_TOAST_LAY] = {"plat overlay toast lay", "toast_draw", "overlay.c", 0, 0},
    [OVERLAY_ZONE_MENU_LAY] = {"plat overlay menu lay", "menu_lay", "overlay.c", 0, 0}};
static void* zone_begin(int zone);
static void zone_value(void* zone, u64 value) { SNX_TRACY_EMIT(TracyCZoneValue(*(TracyCZoneCtx*)zone, value)); }
static void zone_text(void* zone, const char* text) {
    SNX_TRACY_EMIT(TracyCZoneText(*(TracyCZoneCtx*)zone, text, strlen(text)));
}
static void zone_end(void* zone) { snx_tracy_zone_end(*(TracyCZoneCtx*)zone); }
static void plot(const char* name, double value) { SNX_PLOT(name, value); }
#endif

static const SnxMenuHost HOST = {
    .row = row,
    .sections = g_ini_sections,
    .set = ini_set_apply,
    .picked = ini_picked,
    .unpick = ini_unpick,
    .next_start = ini_next_start,
    .picks_changes = ini_picks_changes,
    .old_settings = ini_old_settings,
    .clean_old = ini_clean_old,
    .capture = capture,
    .tas_frames = unattended_slot_frames,
    .tas_recording = unattended_recording,
    .tas_replaying = unattended_replaying,
    .tas_record = unattended_record_from_menu,
    .tas_replay = unattended_replay_from_menu,
    .tv_choice = options_tv_mode_choice,
    .tv_step = options_tv_mode_step,
    .touch_set = backend_touch_set,
    .pad_of_player = gamepad_of_player,
    .pad_name = gamepad_name,
    .pad_layout = gamepad_layout,
    .pad_held = gamepad_held,
    .pad_names_version = gamepad_names_version,
    .pad_store = gamepad_store,
    .pad_map = pad_map,
    .pad_map_set = pad_map_set,
    .preparing = prepare_screen_shown,
    .picture = ui_picture,
    .image = ui_image,
    .image_set = ui_image_set,
    .image_get = ui_image_get,
    .dialog_shown = ctrl_dialog_shown,
    .dialog_draw = ctrl_dialog_draw,
    .hud_box = hud_box_drawn,
#if defined(SNX_TRACY)
    .zone_begin = zone_begin,
    .zone_value = zone_value,
    .zone_text = zone_text,
    .zone_end = zone_end,
    .plot = plot,
#endif
};
static struct {
    SnxMenuHost host; /* HOST with the sections' count, which is no constant expression */
#if defined(SNX_TRACY)
    TracyCZoneCtx zones[sizeof ZONES / sizeof *ZONES]; /* each open one's: the renderer's thread draws the menu */
#endif
} g_overlay_host;
#if defined(SNX_TRACY)
static void* zone_begin(int zone) {
    g_overlay_host.zones[zone] = snx_tracy_zone_begin(&ZONES[zone]);
    return &g_overlay_host.zones[zone];
}
#endif
void overlay_host_nx(void) {
    g_overlay_host.host = HOST;
    g_overlay_host.host.sections_count = g_ini_sections_count;
    overlay_set_host(&g_overlay_host.host);
}
