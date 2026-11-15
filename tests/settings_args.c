/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The settings' names on a command line (tools/settingsgen's settings_args.c, settings_arg): every key that has an _
   is given with a - for each too, listed in the order ini_arg searches, each read back as its key and none a common
   option's name (common/utilities/cli.h), which a driver matches first. A key as it is is read as before; a name with
   some of its _ left, or of no setting, is the key as given, for the reader to refuse by it */
#include "snx_settings.h"
#include "common/utilities/cli.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* settings_arg's key and value for arg, both as expected; a NULL key: no --NAME=value at all */
static void read_as(const char* arg, const char* expected_key, const char* expected_value) {
    char key[64];
    const char* value = settings_arg(arg, key, sizeof key);
    if (!expected_key) {
        assert(!value);
        return;
    }
    assert(value && !strcmp(value, expected_value));
    if (strcmp(key, expected_key)) {
        fprintf(stderr, "%s: key %s, not %s\n", arg, key, expected_key);
        assert(!"settings_arg's key");
    }
}

int main(void) {
    char arg[96], key[64], error[160];
    int i;
    assert(g_setting_args_count > 100);
    for (i = 0; i < g_setting_args_count; i++) {
        const char* name = g_setting_args[i];
        char* one[1] = {arg};
        int at = 0;
        SnxCliArg given;
        const char* value;
        size_t k;
        assert(strchr(name, '-') && !strchr(name, '_'));
        if (i && strcmp(g_setting_args[i - 1], name) >= 0) { /* sorted, each once: the search finds every one */
            fprintf(stderr, "g_setting_args: %s before %s\n", g_setting_args[i - 1], name);
            assert(!"sorted");
        }
        snprintf(arg, sizeof arg, "--%s=1", name);
        value = settings_arg(arg, key, sizeof key);
        assert(value && !strcmp(value, "1") && strlen(key) == strlen(name));
        for (k = 0; name[k]; k++) assert(key[k] == (name[k] == '-' ? '_' : name[k]));
        if (snx_cli_match(1, one, &at, ~0u, "scarletnx", &given, error, sizeof error)) {
            fprintf(stderr, "--%s is a common option's name, which every driver takes before a setting\n", name);
            assert(!"a common option");
        }
    }
    /* each kind of key: the recompile's, a repeat's (<N>), an each's (<ID>, its id with an _ of its own), a Vulkan
       extension's, a key with no _ (one name alone), and the key as it is */
    read_as("--aot-threads=4", "aot_threads", "4");
    assert(ini_find(g_recompile_ini, "aot_threads"));
    read_as("--aot-threads-most=2", "aot_threads_most", "2");
    read_as("--gamepad-p1-swap-ab=1", "gamepad_p1_swap_ab", "1");
    read_as("--gamepad-p8-swap-ab=0", "gamepad_p8_swap_ab", "0");
    read_as("--controls-stick-l=0.5 0.5 100 1", "controls_stick_l", "0.5 0.5 100 1");
    read_as("--controls-stick-l-shown=1", "controls_stick_l_shown", "1");
    read_as("--vk-ext-shader-object=off", "vk_ext_shader_object", "off");
    read_as("--opt=s", "opt", "s");
    read_as("--aot_threads=4", "aot_threads", "4");
    read_as("--state_slots=a=b", "state_slots", "a=b");
    read_as("--state-slots=", "state_slots", "");
    /* not a name: some _ left, a typo, a single dash, no value, one past the key's room */
    read_as("--aot_threads-most=1", "aot_threads-most", "1");
    read_as("--aot-threads_most=1", "aot-threads_most", "1");
    read_as("--aot-thread=1", "aot-thread", "1");
    read_as("-aot-threads=1", NULL, NULL);
    read_as("--aot-threads", NULL, NULL);
    memset(arg, 'a', sizeof arg);
    memcpy(arg, "--", 2);
    memcpy(arg + 2 + sizeof key, "=1", 3);
    read_as(arg, NULL, NULL);
    puts("settings_args passed");
    return 0;
}
