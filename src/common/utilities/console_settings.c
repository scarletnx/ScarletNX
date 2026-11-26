/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/console_settings.h"
#include "snx_settings.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* what a console lacks, said after "... does nothing in a GBA game: " */
#define DOCK          "it has no dock: TV mode is the Switch's"
#define ONE_PROGRAM   "its game is one program"
#define SWITCH_GPU    "it tunes the Switch's GPU"
#define SWITCH_CODE   "it tunes the Switch's 64-bit code"
#define SWITCH_OS     "it is the Switch system's"
#define SWITCH_PADS   "it is the Switch's controllers'"
#define PLAYERS       "it is the Switch's players': any controller plays"
#define NO_HDR        "it has no HDR screen"
#define NO_VIDEO      "it has no video decoder"
#define CPU_SCREEN    "it draws its screen on the CPU"
#define NO_SHADERS    "its GPU runs no shaders of the game's"
#define NO_UPSCALER   "its games have no upscaler of their own"
#define PICA_SHADERS  "its shaders are the PICA's, which snx-shaderc does not read"
#define ONE_PIPELINE  "its picture takes one pipeline"
#define NEAREST       "its GPU samples the nearest texel, with no mipmaps"
#define NO_BUFFERS    "its GPU reads no buffers of the game's"
#define NO_NETWORK    "it has no network"
#define NO_MIC        "it has no microphone"
#define NO_MOTOR      "it has no vibration motor"
#define NO_STICK      "it has no stick: a stick presses its D-pad past half its way"
#define NO_LANGUAGE   "it has no system language"
#define NO_UPDATES    "a cartridge has no updates or DLCs"
#define SWITCH_NET    "it is the Switch's online service: the console's own is in its section"
#define NOT_OFFERED   "it is not offered, and its game plays offline as in flight mode"
#define ONE_USER      "its games play as one user"
#define OWN_ROW       "it has a row of its own in its section"
#define VULKAN_13     "its renderer runs on Vulkan 1.3 only"
#define OWN_FILE      "its driver reads it from the game's folder itself"
/* what is not made yet */
#define SOON          "not made yet"
#define NO_MIC_SAMPLES "its microphone requests are answered with no samples yet"
#define SOON_LOG      "its game log has no levels yet"
#define TRACY_PICTURES "it sends no frame's picture to Tracy yet"
#define TRACY_MEMORY   "its allocations are not in Tracy yet"
#define TRACY_GPU      "its GPU's work is not in Tracy yet"
#define TRACY_LOCKS    "its guest's locks are not in Tracy yet"
#define NO_KERNEL      "its game runs with no kernel, so with no lock of one"

#define NA(text)    {text, false}
#define LATER(text) {text, true}
#define C           SNX_SYSTEM_3DS
#define V           SNX_SYSTEM_VITA
#define G           SNX_SYSTEM_GBA
#define D           SNX_SYSTEM_NDS
/* every console but the Switch the same */
#define OTHERS(why) {[C] = why, [V] = why, [G] = why, [D] = why}

const SnxConsoleSetting g_console_settings[] = {
    /* Game */
    {"source", OTHERS(NA(OWN_FILE))},
    {"addons", {[C] = NA(OWN_FILE), [V] = LATER(SOON), [G] = NA(NO_UPDATES), [D] = NA(NO_UPDATES)}},
    {"mods_off", OTHERS(LATER(SOON))},
    {"cheats_off", OTHERS(LATER(SOON))},
    {.key = "gpu_driver"},
    {.key = "freedreno_env"},
    {.key = "state_slots"},
    {.key = "state_auto_backup"},
    {"save_layout", OTHERS(NA(SWITCH_OS))},
    {"microphone", {[G] = NA(NO_MIC), [D] = LATER(NO_MIC_SAMPLES)}},
    {"mic_noise_reduction", {[G] = NA(NO_MIC), [D] = LATER(NO_MIC_SAMPLES)}},
    /* Display */
    {"docked", OTHERS(NA(DOCK))},
    {"tv_refresh_rate", OTHERS(NA(DOCK))},
    {"tv_resolution", OTHERS(NA(DOCK))},
    {.key = "fullscreen"},
    {"transition_screen", OTHERS(NA(ONE_PROGRAM))},
    {"language", {[G] = NA(NO_LANGUAGE)}},
    {.key = "showcase"},
    {.key = "showcase_corner"},
    {.key = "stats"},
    {.key = "stats_corner"},
    /* Graphics */
    {"resolution", {[C] = NA(OWN_ROW), [G] = NA(CPU_SCREEN)}},
    {"internal_resolution", {[C] = NA(OWN_ROW), [V] = NA(NO_UPSCALER), [G] = NA(CPU_SCREEN), [D] = NA(NO_UPSCALER)}},
    {"render_upscaler", OTHERS(NA(SWITCH_GPU))},
    {"render_anti_aliasing", OTHERS(NA(SWITCH_GPU))},
    {"smaa_quality", OTHERS(NA(SWITCH_GPU))},
    {"smaa_edges", OTHERS(NA(SWITCH_GPU))},
    {"jitter", OTHERS(NA(SWITCH_GPU))},
    {"fsr_sharpness", OTHERS(NA(SWITCH_GPU))},
    {"dlss_runtime", OTHERS(NA(SWITCH_GPU))},
    {"dlss_preset", OTHERS(NA(SWITCH_GPU))},
    {"shadows", OTHERS(NA(SWITCH_GPU))},
    {.key = "vsync"},
    {.key = "frame_pacing"},
    {"anisotropy", {[G] = NA(CPU_SCREEN), [D] = NA(NEAREST)}},
    {.key = "upscaler"},
    {.key = "upscaler_sharpness"},
    {.key = "anti_aliasing"},
    {"hdr_game", OTHERS(NA(NO_HDR))},
    {"hdr_paperwhite", OTHERS(NA(NO_HDR))},
    {"hdr_peak", OTHERS(NA(NO_HDR))},
    {"vulkan_video", {[C] = LATER(SOON), [G] = NA(NO_VIDEO), [D] = NA(NO_VIDEO)}},
    {"guest_vulkan", OTHERS(NA(SWITCH_GPU))},
    {"apple_video", {[C] = LATER(SOON), [V] = LATER(SOON), [G] = NA(NO_VIDEO), [D] = NA(NO_VIDEO)}},
    {"vulkan_driver", OTHERS(LATER(SOON))},
    {"vertex_input_dynamic_state", OTHERS(NA(SWITCH_GPU))},
    {.key = "vulkan_ban_ignore"},
    {"astc_mode", OTHERS(NA(SWITCH_GPU))},
    {"texture_shed", OTHERS(NA(SWITCH_GPU))},
    {"texture_half_res", OTHERS(NA(SWITCH_GPU))},
    {"astc_lines_bc1", OTHERS(NA(SWITCH_GPU))},
    {"texture_sysmem", OTHERS(NA(SWITCH_GPU))},
    {"bc7_queue", OTHERS(NA(SWITCH_GPU))},
    {"astc_compute_queue", OTHERS(NA(SWITCH_GPU))},
    {"astc_quick_mtexels", OTHERS(NA(SWITCH_GPU))},
    {"astc_background", OTHERS(NA(SWITCH_GPU))},
    {.key = "vk_<extension>"},
    /* Audio */
    {.key = "volume"},
    /* Performance */
    {.key = "fps_limit"},
    {"frame_rate", OTHERS(NA(SWITCH_OS))},
    {"memory_gb", OTHERS(NA(SWITCH_OS))},
    {"memory_fit_mode", OTHERS(NA(SWITCH_OS))},
    {"heap_max_mb", OTHERS(NA(SWITCH_OS))},
    {"map_limit_mb", OTHERS(NA(SWITCH_OS))},
    {"map_limit_files", OTHERS(NA(SWITCH_OS))},
    {"thread_stack_mb", OTHERS(NA(SWITCH_OS))},
    {.key = "gpu_pstate"},
    {"pipeline_threads", {[G] = NA(ONE_PIPELINE)}},
    {"async_pipelines", {[G] = NA(ONE_PIPELINE)}},
    {"pipeline_libraries", {[G] = NA(NO_SHADERS), [D] = NA(NO_SHADERS)}},
    {"prepare_shaders", {[G] = NA(NO_SHADERS), [D] = NA(NO_SHADERS)}},
    {"compress_prepared_shaders", {[G] = NA(NO_SHADERS), [D] = NA(NO_SHADERS)}},
    {"nce", OTHERS(NA(SWITCH_CODE))},
    {"jit_threads", OTHERS(NA(SWITCH_CODE))},
    {"jit_region_instructions", OTHERS(NA(SWITCH_CODE))},
    {"jit_live_budget", OTHERS(NA(SWITCH_CODE))},
    {"jit_opt", OTHERS(LATER(SOON))},
    /* On-screen controls and touch (the controls' own finder, vpad.c, reads them first) */
    {.key = "controls"},
    {.key = "controls_snap"},
    {.key = "controls_scale"},
    {.key = "controls_rel_center"},
    {.key = "controls_dpad_slide"},
    {.key = "controls_haptics"},
    {.key = "controls_<ID>_shown"},
    {.key = "controls_<ID>"},
    {.key = "touch"},
    /* Controllers */
    {.key = "gamepad_swap_ab"},
    {"gamepad_deadzone", {[G] = NA(NO_STICK), [D] = NA(NO_STICK)}},
    {"gamepad_rumble", {[C] = NA(NO_MOTOR), [V] = NA(NO_MOTOR)}},
    {.key = "gamepad_background"},
    {"gamepad_joycon_pair", OTHERS(NA(SWITCH_PADS))},
    {"gamepad_assign", OTHERS(NA(PLAYERS))},
    {"controller_applet", OTHERS(NA(SWITCH_PADS))},
    {"game_mouse", OTHERS(NA(SWITCH_PADS))},
    {.key = "gamepad_map"},
    {"gamepad_p<N>_device", OTHERS(NA(PLAYERS))},
    {"gamepad_p<N>_connected", OTHERS(NA(PLAYERS))},
    {"gamepad_p<N>_type", OTHERS(NA(PLAYERS))},
    {"gamepad_p<N>_swap_ab", OTHERS(NA(PLAYERS))},
    {"gamepad_p<N>_rumble", OTHERS(NA(PLAYERS))},
    {"gamepad_p<N>_map", OTHERS(NA(PLAYERS))},
    {"joycon2", OTHERS(NA(SWITCH_PADS))},
    {"joycon2_mouse_scale", OTHERS(NA(SWITCH_PADS))},
    /* Shortcuts (the runtime's keys, hotkeys.c, read them first) */
    {.key = "hotkey_help"},
    {.key = "hotkey_menu"},
    {.key = "hotkey_screenshot"},
    {.key = "hotkey_fps_limit"},
    {.key = "hotkey_thread_dump"},
    {.key = "hotkey_showcase"},
    {.key = "hotkey_save_state"},
    {.key = "hotkey_load_state"},
    {.key = "hotkey_next_slot"},
    {.key = "hotkey_tracy"},
    {.key = "hotkey_pause"},
    {.key = "hotkey_fullscreen"},
    {.key = "hotkey_fullscreen_2"},
    {.key = "hotkey_stats"},
    {.key = "hotkey_capture"},
    {.key = "hotkey_mouse"},
    {.key = "hotkey_quit"},
    {.key = "hotkey_tv_mode"},
    {.key = "hotkey_upscaler"},
    {.key = "hotkey_anti_aliasing"},
    /* Network */
    {"network", {[V] = NA(NOT_OFFERED), [G] = NA(NO_NETWORK)}},
    {"airplane_mode", {[V] = NA(NOT_OFFERED), [G] = NA(NO_NETWORK)}},
    {"ldn", {[V] = NA(NOT_OFFERED), [G] = NA(NO_NETWORK), [D] = LATER(SOON)}},
    {"room", {[V] = NA(NOT_OFFERED), [G] = NA(NO_NETWORK), [D] = LATER(SOON)}},
    {"switch_online", OTHERS(NA(SWITCH_NET))},
    {"user_uid", OTHERS(NA(ONE_USER))},
    /* Debug */
    {"fsr_input_conventions", OTHERS(NA(SWITCH_GPU))},
    {"log", OTHERS(LATER(SOON_LOG))},
    {"emulated_cores_mode", OTHERS(NA(SWITCH_OS))},
    {.key = "log_max"},
    {"auto_stub_ipc", {[C] = NA(OWN_ROW), [V] = NA(SWITCH_OS), [G] = NA(SWITCH_OS), [D] = NA(SWITCH_OS)}},
    {"diag", OTHERS(NA(SWITCH_GPU))},
    {.key = "vk_validation"},
    {"vk_pipeline_cache", {[G] = NA(ONE_PIPELINE)}},
    {"vk_mirror_past_limit", OTHERS(NA(SWITCH_GPU))},
    {"vk_api", OTHERS(NA(VULKAN_13))},
    {"native_astc", OTHERS(NA(SWITCH_GPU))},
    {"native_bc", OTHERS(NA(SWITCH_GPU))},
    {"native_etc2", OTHERS(NA(SWITCH_GPU))},
    {"bcn_astc", OTHERS(NA(SWITCH_GPU))},
    {"cpu_extensions", OTHERS(NA(SWITCH_CODE))},
    {.key = "slp_vectorizer"},
    {"fpsr_flags", OTHERS(NA(SWITCH_CODE))},
    {"abi_contract", OTHERS(NA(SWITCH_CODE))},
    {"volatile_memory", OTHERS(NA(SWITCH_CODE))},
    {"ir_stats", OTHERS(NA(SWITCH_CODE))},
    {"ir_dump_pc", OTHERS(NA(SWITCH_CODE))},
    {"ir_branch_profile", OTHERS(NA(SWITCH_CODE))},
    {.key = "gdb_jit"},
    {"alias_pool", OTHERS(NA(SWITCH_OS))},
    {"guest_files", OTHERS(NA(SWITCH_OS))},
    {"write_tracking", OTHERS(NA(SWITCH_OS))},
    {"write_tracking_hot_pages", OTHERS(NA(SWITCH_OS))},
    {"memory_traps", OTHERS(NA(SWITCH_OS))},
    {"host_page", OTHERS(NA(SWITCH_OS))},
    {"nce_foreign_scan", OTHERS(NA(SWITCH_CODE))},
    {"gpu_page_check", OTHERS(NA(SWITCH_GPU))},
    {"draw_state_check", OTHERS(NA(SWITCH_GPU))},
    {"pool_map_shadow", OTHERS(NA(SWITCH_GPU))},
    {"tex_events_shadow", OTHERS(NA(SWITCH_GPU))},
    {"pool_map", OTHERS(NA(SWITCH_GPU))},
    {"writer_barriers", OTHERS(NA(SWITCH_GPU))},
    {"profile", OTHERS(LATER(SOON))},
    {"hooks", {[C] = NA(OWN_ROW), [V] = NA(SWITCH_OS), [G] = NA(SWITCH_OS), [D] = NA(SWITCH_OS)}},
    {"hooks_off", OTHERS(NA(SWITCH_OS))},
    {"hook_modules_dir", OTHERS(NA(SWITCH_OS))},
    {"hooks_verify", OTHERS(NA(SWITCH_OS))},
    {"native_cond_var", OTHERS(NA(SWITCH_OS))},
    {"native_os_sync", OTHERS(NA(SWITCH_OS))},
    {"gpu_hook_trace", OTHERS(NA(SWITCH_GPU))},
    {"gpu_hook_trace_from", OTHERS(NA(SWITCH_GPU))},
    {"gpu_hook_trace_words", OTHERS(NA(SWITCH_GPU))},
    {"shader_corpus", {[C] = NA(PICA_SHADERS), [V] = NA(OWN_ROW), [G] = NA(NO_SHADERS), [D] = NA(NO_SHADERS)}},
    {"pipeline_stats", OTHERS(NA(SWITCH_GPU))},
    {"gl_call_census", OTHERS(NA(SWITCH_GPU))},
    {"astc_dump", OTHERS(NA(SWITCH_GPU))},
    {"sm86_dump", OTHERS(NA(SWITCH_GPU))},
    {"gpu_trace_target", OTHERS(NA(SWITCH_GPU))},
    {.key = "vk_gpu_profile"},
    {.key = "vk_checkpoints"},
    {.key = "vk_debug_hide_commands"},
    {"vk_mesh_path", OTHERS(NA(SWITCH_GPU))},
    {"vk_memory", {[V] = LATER(SOON), [G] = NA(CPU_SCREEN), [D] = NA(NO_BUFFERS)}},
    {"uma_memory", OTHERS(NA(SWITCH_GPU))},
    {"uma_copies", OTHERS(NA(SWITCH_GPU))},
    {"vk_buffer_stats", OTHERS(NA(SWITCH_GPU))},
    {"vk_draw_runs", OTHERS(NA(SWITCH_GPU))},
    {"vk_sparse_share", OTHERS(NA(SWITCH_GPU))},
    {"vk_stage_memory", OTHERS(NA(SWITCH_GPU))},
    {"vk_buffer_pages", OTHERS(NA(SWITCH_GPU))},
    {"vk_stream_ring", OTHERS(NA(SWITCH_GPU))},
    {"vk_buffer_slabs", OTHERS(NA(SWITCH_GPU))},
    {"nvn_storage_order", OTHERS(NA(SWITCH_GPU))},
    {"vk_descriptors_mode", OTHERS(NA(SWITCH_GPU))},
    {"vk_cb_address", OTHERS(NA(SWITCH_GPU))},
    {"vk_cb_windows", OTHERS(NA(SWITCH_GPU))},
    {"vk_dynamic_cbufs", OTHERS(NA(SWITCH_GPU))},
    {"vk_copy_indirect", OTHERS(NA(SWITCH_GPU))},
    {"vk_inplace_loads", OTHERS(NA(SWITCH_GPU))},
    {"vk_record", OTHERS(NA(SWITCH_GPU))},
    {"gpu_split", OTHERS(NA(SWITCH_GPU))},
    {"compute", OTHERS(NA(SWITCH_GPU))},
    {"hmma_f16", OTHERS(NA(SWITCH_GPU))},
    {"hmma_mode", OTHERS(NA(SWITCH_GPU))},
    {"hmma_narrow_lds", OTHERS(NA(SWITCH_GPU))},
    {"vk_max_allocation_mb", OTHERS(NA(SWITCH_GPU))},
    {"vk_early_copy_back", OTHERS(NA(SWITCH_GPU))},
    {"vk_early_landing", OTHERS(NA(SWITCH_GPU))},
    {"vk_submit_every", OTHERS(NA(SWITCH_GPU))},
    {"vk_budget_mb", OTHERS(NA(SWITCH_GPU))},
    {.key = "renderdoc"},
    {.key = "renderdoc_frames"},
    {.key = "tracy_profiler"},
    {.key = "tracy_capture"},
    {.key = "tracy_capture_seconds"},
    {.key = "tracy_sampling"},
    {.key = "tracy_context_switches"},
    {"tracy_frame_images", OTHERS(LATER(TRACY_PICTURES))},
    {.key = "tracy_port"},
    {.key = "tracy_capture_tool"},
    {.key = "tracy_open_trace"},
    {.key = "tracy_zone_callstack"},
    {"tracy_memory_detail", OTHERS(LATER(TRACY_MEMORY))},
    {"tracy_memory_callstack", OTHERS(LATER(TRACY_MEMORY))},
    {"tracy_lock_marks",
     {[C] = LATER(TRACY_LOCKS), [V] = LATER(TRACY_LOCKS), [G] = NA(NO_KERNEL), [D] = NA(NO_KERNEL)}},
    {"tracy_gpu_zone_callstack", OTHERS(LATER(TRACY_GPU))},
    {.key = "perf_map"},
    {.key = NULL},
};

const char* snx_console_name(SnxSystem console) {
    static const char* const names[SNX_SYSTEMS] = {"Switch", "3DS", "Vita", "GBA", "DS"};
    return console >= 0 && console < SNX_SYSTEMS ? names[console] : "?";
}
/* a console's own row, its key's prefix its console's, which none of the lines above has */
static bool own_row(const char* key) {
    static const char* const prefixes[] = {"ctr_", "vita_", "nds_", "gba_", "linux_", "android_"};
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; ++i)
        if (!strncmp(key, prefixes[i], strlen(prefixes[i]))) return true;
    return false;
}
/* key as a line's pattern has it: <N> its digits, <ID> one of the list's ids (small letters, digits and _), the
   pattern's text around them as it is; vk_<extension> a row of the Vulkan Extensions table */
static bool matches(const char* pattern, const char* key) {
    if (!strcmp(pattern, "vk_<extension>")) {
        for (int i = 0; i < g_vk_extension_settings_count; ++i) { /* its key the extension's name lowercased */
            const char *name = g_vk_extension_settings[i].name, *k = key;
            while (*name && *k && tolower((unsigned char)*name) == *k) ++name, ++k;
            if (!*name && !*k) return true;
        }
        return false;
    }
    const char* hole = strchr(pattern, '<');
    if (!hole) return !strcmp(pattern, key);
    const char* close = strchr(hole, '>');
    if (!close) return false;
    const char* after = close + 1;
    size_t head = (size_t)(hole - pattern), tail = strlen(after), n = strlen(key);
    if (n <= head + tail || strncmp(key, pattern, head) || strcmp(key + n - tail, after)) return false;
    for (const char* c = key + head; c < key + n - tail; ++c)
        if (!strncmp(hole, "<N>", 3) ? !isdigit((unsigned char)*c)
                                     : !(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_'))
            return false;
    return true;
}
const SnxConsoleSetting* snx_console_setting(const char* key) {
    if (!key || own_row(key)) return NULL;
    for (int i = 0; g_console_settings[i].key; ++i) /* the lines without a pattern first: controls_snap */
        if (!strchr(g_console_settings[i].key, '<') && !strcmp(g_console_settings[i].key, key))
            return &g_console_settings[i];
    for (int i = 0; g_console_settings[i].key; ++i)
        if (strchr(g_console_settings[i].key, '<') && matches(g_console_settings[i].key, key))
            return &g_console_settings[i];
    return NULL;
}
const SnxConsoleWhy* snx_console_why(SnxSystem console, const char* key) {
    static const SnxConsoleWhy none = NA("it is no setting the consoles share");
    const SnxConsoleSetting* s = snx_console_setting(key);
    if (console == SNX_SYSTEM_SWITCH) return s ? NULL : &none;
    if (!s || console < 0 || console >= SNX_SYSTEMS) return &none;
    return s->why[console].text ? &s->why[console] : NULL;
}
const IniOpt* snx_console_opt(SnxSystem console, const char* key) {
    return snx_console_why(console, key) ? NULL : ini_find(g_console_ini, key);
}
int snx_console_arg(SnxSystem console, const char* arg, char* error, size_t size) {
    char key[64];
    const char* value = settings_arg(arg, key, sizeof key);
    if (!value || !ini_find(g_console_ini, key) || own_row(key)) return 0;
    const SnxConsoleWhy* why = snx_console_why(console, key);
    if (why) {
        snprintf(error, size, "%s does nothing in a %s game: %s", arg, snx_console_name(console), why->text);
        return -1;
    }
    ini_read(ini_find(g_console_ini, key), value); /* a value the row does not take said with the one it keeps */
    return 1;
}
