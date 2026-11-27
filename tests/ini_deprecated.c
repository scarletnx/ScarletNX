/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The .ini's deprecated keys (src/nx/runtime/ini.c, docs/user/deprecations.md): which keys they are and what became of each,
   the [Deprecated] section as the runtime writes it (the launcher's Clean Old Settings reads it), and the line a run
   says about them */
#include "nx/runtime/ini.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const char expected[] =
        "\n[Deprecated]\n; keys an older version used, which do nothing now (docs/user/deprecations.md): delete them, or use "
        "Clean Old Settings\n;   in the game's menu or the launcher's Settings > App\n; renderer: Vulkan is the only renderer\n"
        "renderer=opengl\n; touch_controls: now controls\ntouch_controls=1\n";
    const char* keys[] = {"renderer", "touch_controls"};
    char text[1024], small[40];
    size_t n;
    FILE* f;
    /* the keys: the OpenGL renderer's, the touch controls' old names, the first Spanish ones; a setting is none */
    assert(!strcmp(ini_deprecated("renderer"), "Vulkan is the only renderer"));
    assert(!strcmp(ini_deprecated("lsfg_flow"), "LSFG went with the OpenGL renderer"));
    assert(!strcmp(ini_deprecated("touch_controls"), "now controls") &&
           !strcmp(ini_deprecated("modo_tv"), "now docked"));
    assert(!strcmp(ini_deprecated("user"), "now user_uid, the uid of the user it named") &&
           !ini_deprecated("user_uid"));
    assert(ini_deprecated("vk_descriptors") && strstr(ini_deprecated("vk_descriptors"), "now vk_descriptors_mode") &&
           !ini_deprecated("vk_descriptors_mode"));
    /* write_tracking_hot: its 0, the default every .ini wrote, is write_tracking_hot_pages's Auto, so not a rename */
    assert(ini_deprecated("write_tracking_hot") &&
           strstr(ini_deprecated("write_tracking_hot"), "now write_tracking_hot_pages, whose 2 is Auto") &&
           !ini_deprecated("write_tracking_hot_pages") && !ini_renamed("write_tracking_hot"));
    /* emulated_cores: its 1, the default every .ini wrote, is emulated_cores_mode's Auto */
    assert(ini_deprecated("emulated_cores") &&
           strstr(ini_deprecated("emulated_cores"),
                  "now emulated_cores_mode, of the same 0 (Off) and 1 (On) and 2, Auto") &&
           !ini_deprecated("emulated_cores_mode"));
    /* of the same 0 and 1: a pick of it (the launcher's, the game's) is carried over */
    assert(ini_renamed("emulated_cores") && !strcmp(ini_renamed("emulated_cores"), "emulated_cores_mode"));
    /* renamed, of the same values: the launcher carries a pick of it over */
    assert(ini_renamed("vk_descriptors") && !strcmp(ini_renamed("vk_descriptors"), "vk_descriptors_mode") &&
           !ini_renamed("memory_fit") && !ini_renamed("vk_descriptors_mode") && !ini_renamed(""));
    assert(!strcmp(ini_renamed("dlss_render_size"), "internal_resolution") && ini_deprecated("dlss_render_size") &&
           ini_deprecated("dlss_upscaler") && !ini_renamed("dlss_upscaler") && !ini_deprecated("internal_resolution"));
    /* the NVN and Maxwell paths' switches, the ASTC background's CPU threads and the unattended run's one press */
    assert(!strcmp(ini_deprecated("nvn_direct"), "the runtime always runs the game's NVN calls") &&
           ini_deprecated("mme_verify") && ini_deprecated("astc_background_threads") &&
           !ini_deprecated("astc_background"));
    assert(!strcmp(ini_deprecated("press_at"), "now script, whose press_at:press is the same press") &&
           !ini_deprecated("press_len") && !ini_deprecated("script"));
    /* the mesh draws follow their extension's setting now: what an old .ini's mesh_draws=0 meant is not taken */
    assert(ini_deprecated("mesh_draws") && strstr(ini_deprecated("mesh_draws"), "vk_ext_mesh_shader=off") &&
           !ini_deprecated("vk_ext_mesh_shader"));
    assert(!ini_deprecated("resolution") && !ini_deprecated("controls") && !ini_deprecated(""));
    /* LLVM's: its keys by name, its passes and options by their prefixes, where a prefix alone is no key */
    assert(!strcmp(ini_deprecated("opt"),
                   "went with LLVM: the native back ends make the game's code, with no optimization level") &&
           ini_deprecated("jit_opt") && ini_deprecated("ir_branch_profile") && !ini_deprecated("aot_threads"));
    assert(
        !strcmp(ini_deprecated("llvm_pass_instcombine"), "went with LLVM's passes: the native back ends run none") &&
        !strcmp(ini_deprecated("llvm_inliner_threshold"), "went with LLVM's options: the native back ends have none") &&
        !ini_deprecated("llvm_") && !ini_deprecated("llvm"));
    /* the section, word for word */
    f = tmpfile();
    assert(f);
    ini_deprecated_head(f);
    ini_deprecated_key(f, "renderer", "opengl");
    ini_deprecated_key(f, "touch_controls", "1");
    rewind(f);
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    assert(!strcmp(text, expected));
    /* the line: none says nothing; one and more, named; cut to the room it has */
    assert(!ini_deprecated_note(text, sizeof text, "Game.ini", keys, 0));
    assert(ini_deprecated_note(text, sizeof text, "Game.ini", keys, 1));
    assert(!strcmp(text, "settings: Game.ini keeps 1 key this version does not use (renderer), under [Deprecated]: "
                         "delete it, or use Clean Old Settings in the game's menu or the launcher's Settings > App"));
    assert(ini_deprecated_note(text, sizeof text, "Game.ini", keys, 2) &&
           strstr(text, "keeps 2 keys this version does not use (renderer, touch_controls)") &&
           strstr(text, "delete them"));
    assert(ini_deprecated_note(small, sizeof small, "Game.ini", keys, 2) && strlen(small) == sizeof small - 1);
    puts("ini_deprecated passed");
    return 0;
}
