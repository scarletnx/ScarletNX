# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# `scarletnx --settings`: every setting of the game's .ini, by section, a line each of ten fields, the help on one
# line, the menus' name and values (runtime/options.c's ini_list), where the setting does nothing the systems it is
# for (ini_only), and, on the rows that need a build, that build; a slider's (INI_SLIDER) slider in a twelfth, its
# every step listed and its default one of them. Run by ctest as runtime_settings_list
execute_process(COMMAND "${RUNTIME}" --settings RESULT_VARIABLE result OUTPUT_VARIABLE list ERROR_VARIABLE errors)
if(result OR NOT errors STREQUAL "")
    message(FATAL_ERROR "scarletnx --settings ended with ${result}: ${errors}")
endif()
set(field "[^\t\r\n]*")
string(REGEX REPLACE "${field}\t${field}\t${field}\t(int|float|text)\t${field}\t${field}\t${field}\t${field}\t${field}\t${field}(\t${field}(\t(slider|open|slider open|boxes|folder|file|save))?)?\r?\n"
       "" rest "${list}")
if(NOT rest STREQUAL "" OR list STREQUAL "")
    message(FATAL_ERROR "lines of scarletnx --settings that are not ten, eleven or twelve fields:\n${rest}")
endif()
# a slider's steps are the words the launcher's row goes along, its default the step it starts on: each listed, the
# default among them
string(REGEX MATCHALL "\n[^\t\n]*\t[^\t\n]*\t[^\t\n]*\t[^\t\n]*\t[^\t\n]*\t[^\t\n]*\t[^\t\n]*\t[^\t\n]*\t[^\t\n]*\t[^\t\n]*\t[^\t\n]*\tslider" sliders "${list}")
if(NOT sliders)
    message(FATAL_ERROR "scarletnx --settings lists no slider")
endif()
foreach(row IN LISTS sliders)
    string(REGEX REPLACE "^\n[^\t]*\t([^\t]*)\t([^\t]*)\t[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t([^\t]*)\t.*" "\\1;\\2;\\3" parts "${row}")
    list(GET parts 0 key)
    list(GET parts 1 default)
    list(GET parts 2 steps)
    if(steps STREQUAL "" OR NOT "|${steps}" MATCHES "\\|${default}=")
        message(FATAL_ERROR "scarletnx --settings: the slider ${key} lists no steps, or not its default ${default}: \"${steps}\"")
    endif()
endforeach()
# the rows this build has none of what they need, named: a menu shows them greyed and picks nothing
set(needed)
string(REGEX MATCHALL "\n[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\t[^\t]*\tneeds-build" rows "${list}")
foreach(row IN LISTS rows)
    string(REGEX REPLACE "^\n[^\t]*\t([^\t]*)\t.*" "\\1" key "${row}")
    list(APPEND needed ${key})
endforeach()
# every row the profiler gates, by its key: a build with the client has none of them saying needs-build, and one
# without it has every one of them saying so. The rows come from the list itself, so a row added takes the check with it
string(REGEX MATCHALL "\n(Debug|Shortcuts)\t(tracy_[a-z_]+|hotkey_tracy)\t" gated "${list}")
foreach(row ${gated})
    string(REGEX REPLACE "^\n(Debug|Shortcuts)\t(tracy_[a-z_]+|hotkey_tracy)\t$" "\\2" key "${row}")
    if(SNX_TRACY)
        if(key IN_LIST needed)
            message(FATAL_ERROR "scarletnx --settings: ${key} says it needs a build this one has")
        endif()
    elseif(NOT key IN_LIST needed)
        message(FATAL_ERROR "scarletnx --settings: ${key} does not say that a build without the client has none of it")
    endif()
endforeach()
if(NOT gated)
    message(FATAL_ERROR "scarletnx --settings lists no Debug row the client gates")
endif()
# the game's menu's settings with its names and words, as the launcher shows them; a whole number of few values with
# its numbers
foreach(row "Display\tdocked\t1\tint\t0\t2\t1 = TV mode" "Display\tfullscreen\t0\tint\t0\t1\t"
            "Graphics\tresolution\t1\tfloat\t0.25\t16\t" "Performance\tfps_limit\tgame\ttext\t"
            "On-screen controls\tcontrols\t" "\tShow controls\t0=Off|1=On|2=Auto\t\n"
            "\tScale\t0=50%|10=60%|20=70%|30=80%|40=90%|50=100%|60=110%|70=120%|80=130%|90=140%|100=150%\t\n"
            "On-screen controls\ttouch\t2\tint\t0\t2\t" "\tTouch screen\t0=Off|1=Fingers|2=Fingers and mouse\t\n"
            "On-screen controls/Buttons\tcontrols_a_shown\t\tint\t0\t1\t" "\tA\t0=Hidden|1=Shown\t\n"
            "Controllers\tgamepad_swap_ab\t0\tint\t0\t1\t" "\tSwap A/B\t0=Off|1=On\t\n"
            "Controllers/Button map\tgamepad_map\t\ttext\t"
            "\tStick dead zone\t0=0%|5=5%|10=10%|" "|90=90%\t\n" "\tVibration\t0=Off|10=10%|20=20%|"
            "Debug\tlog\t1\tint\t0\t3\t" "\tLog level\t0=0|1=1|2=2|3=3\t\n"
            # the game's Volume, a slider of every 5 percent
            "Audio\tvolume\t100\tint\t0\t100\tHow loud the game plays, in percent of its own sound: 0 = mute. "
            "\tVolume\t0=Mute|5=5%|10=10%|15=15%|20=20%|25=25%|30=30%|35=35%|40=40%|45=45%|50=50%|55=55%|60=60%|65=65%|70=70%|75=75%|80=80%|85=85%|90=90%|95=95%|100=100%\t\t\tslider\n"
            "Performance\tprepare_shaders\t2\tint\t0\t2\t"
            "\tPrepare shaders\t0=Off|1=While playing|2=Before starting\t\n"
            ", 2 French, 3 German, 4 Italian, 5 Spanish, 6 Chinese, 7 Korean,"
            # a Mac's Vulkan driver, listed on every system as every row is, its last field naming macOS elsewhere (greyed there)
            "Graphics\tvulkan_driver\tauto\ttext\t"
            "\tVulkan driver\tauto=Auto|moltenvk=MoltenVK|kosmickrisp=KosmicKrisp (experimental)\t")
    string(FIND "${list}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings has no \"${row}\"")
    endif()
endforeach()
# TV mode's Auto, the third value docked takes, as the launcher's Settings offer it
# Vulkan driver's last field: empty on a Mac, "macOS only" on any other system
if(APPLE_HOST)
    set(vk_only "")
else()
    set(vk_only "macOS only")
endif()
if(NOT list MATCHES "\tVulkan driver\t[^\t\n]*\t${vk_only}\r?\n")
    message(FATAL_ERROR "scarletnx --settings does not end Vulkan driver's line with \"${vk_only}\"")
endif()
if(NOT list MATCHES "\nDisplay\tdocked\t[^\n]*\tTV mode\t0=Off\\|1=On\\|2=Auto\t\r?\n")
    message(FATAL_ERROR "scarletnx --settings does not offer TV mode's Auto")
endif()
# a later player's rows have their own help, its first player's: not the Button map's of the player before
if(NOT list MATCHES "\tgamepad_p2_device\t[^\n]*\tPlayer N's controller" OR list MATCHES "\tgamepad_p2_device\t[^\n]*\tPlayer N's map")
    message(FATAL_ERROR "scarletnx --settings gives gamepad_p2_device another setting's help")
endif()
string(FIND "${list}" "\tcontrols_a\t" at) # a control's place: the game's own, set in its edit mode
if(NOT at LESS 0)
    message(FATAL_ERROR "scarletnx --settings lists controls_a, the game's own")
endif()
if(NOT list MATCHES
   "^Game\t.*\nDisplay\t.*\nGraphics\t.*\nAudio\t.*\nPerformance\t.*\nOn-screen controls\t.*\nOn-screen controls/Buttons\t.*\nControllers\t.*\nControllers/Button map\t.*\nNetwork\t.*\nDebug\t")
    message(FATAL_ERROR "scarletnx --settings lists the sections out of the .ini's order")
endif()
if(list MATCHES "\nOn-screen controls/Buttons\t[^\n]*\nOn-screen controls\t") # a page after its section's others
    message(FATAL_ERROR "scarletnx --settings lists a page's settings before its section's last")
endif()
# the 3DS driver's debug settings, on the 3DS group's Debug page with their names (the launcher passes each one picked
# to a 3DS game's run); its scripted input, a run's own, on no page
foreach(row "\n3DS\tctr_model\tauto\ttext\t" "\tConsole\tauto=Auto|old=Old 3DS|new=New 3DS\t\n"
            "\n3DS/Debug\tctr_hooks\t1\tint\t0\t1\t" "\tIPC hooks\t0=Off|1=On\t\n"
            "\n3DS/Debug\tctr_stand_in\t1\tint\t0\t2\t" "\tStand-in shaders\t0=Off|1=On|2=Every draw\t\n"
            "\n3DS/Debug\tctr_auto_stub_ipc\t1\tint\t0\t1\t")
    string(FIND "${list}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings has no \"${row}\"")
    endif()
endforeach()
string(FIND "${list}" "\tctr_script\t" at)
if(NOT at LESS 0)
    message(FATAL_ERROR "scarletnx --settings lists ctr_script, a 3DS run's scripted input")
endif()
# the Vita driver's, on the Vita group's Debug page (the launcher passes each one picked to a Vita game's run); the
# lockstep rows only a build configured with -DSNX_VITA_LOCKSTEP=ON has, greyed in any other; its scripted input and the
# press and screenshot rows that go with it, a run's own, on no page
foreach(row "\nVita/Debug\tvita_hle_trace\t0\tint\t0\t2\t" "\tImport trace\t0=Off|1=Missing|2=All\t\n"
            "\nVita/Debug\tvita_gxm_trace\t0\tint\t0\t1\t")
    string(FIND "${list}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings has no \"${row}\"")
    endif()
endforeach()
# the Linux, Android and DS groups: each its driver's rows, the Debug page's after them
foreach(row "\nLinux\tlinux_isolated\t0\tint\t0\t1\t" "\tIsolated\t0=Off|1=On\t\n"
            "\nLinux\tlinux_root\t\ttext\t0\t0\t" "\tRoot folder\t\t\t\tfolder\n"
            "\nLinux\tlinux_jit_cache\t1\tint\t0\t1\t"
            "\nLinux/Debug\tlinux_strace\t0\tint\t0\t1\t"
            "\nAndroid\tandroid_java_options\t\ttext\t0\t0\t" "\tJava options\t\t\n"
            "\nDS\tnds_aot\t1\tint\t0\t1\t" "\nDS/Debug\tnds_wav\t\ttext\t0\t0\t" "\tSound file\t\t\t\tsave\n")
    string(FIND "${list}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings has no \"${row}\"")
    endif()
endforeach()
foreach(key vita_script vita_press_len vita_shot_every)
    string(FIND "${list}" "\t${key}\t" at)
    if(NOT at LESS 0)
        message(FATAL_ERROR "scarletnx --settings lists ${key}, a Vita run's own")
    endif()
endforeach()
foreach(key vita_lockstep vita_lockstep_steps)
    if(NOT list MATCHES "\nVita/Debug\t${key}\t")
        message(FATAL_ERROR "scarletnx --settings has no Vita/Debug row ${key}")
    endif()
    if(SNX_VITA_LOCKSTEP AND key IN_LIST needed)
        message(FATAL_ERROR "scarletnx --settings: ${key} says it needs a build this one has")
    elseif(NOT SNX_VITA_LOCKSTEP AND NOT key IN_LIST needed)
        message(FATAL_ERROR "scarletnx --settings: ${key} does not say that a build without lockstep has none of it")
    endif()
endforeach()
# a number's common values, each with its word, as the launcher offers them and keeps a value between two (open);
# the FPS limit's, a text's, open the same way; the heap's limit a whole number of MB
foreach(row "\nGraphics\tbc7_queue\t0\tint\t0\t16384\t" "\tBC7 queue\t0=Auto|256=256|512=512|1024=1024|2048=2048|4096=4096|8192=8192|16384=16384\t\t\topen\n"
            "\tHost page size\t0=Host's|8192=8192 bytes|16384=16384 bytes|" "|1048576=1048576 bytes\t\t\topen\n"
            "\nPerformance\tfps_limit\tgame\ttext\t" "\tFPS limit\tgame=Game's|0=No limit|30=30 FPS|" "|240=240 FPS\t\t\topen\n"
            "\nDebug\tshader_corpus\t\ttext\t0\t0\t" "\tShader corpus\t\t\t\tfolder\n"
            "\nDebug\tpipeline_stats\t\ttext\t0\t0\t" "\tPipeline stats\t\t\t\tsave\n"
            "\nGame\tmicrophone\toff\ttext\t" "\tMicrophone\t=System default|off=Off\t\t\topen\n"
            "\nPerformance\theap_max_mb\t0\tint\t0\t1048576\t" "\tHeap limit (MB)\t0=No limit|512=512 MB|")
    string(FIND "${list}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings has no \"${row}\"")
    endif()
endforeach()
# a setting of boxes: its items as the launcher offers them, value=Label~About, and boxes as its mark; FSR's input
# conventions' seven bits, the extension bans' keys (each with what the bans on it say), the hook families and the
# Vulkan commands of the extensions a renderer can use, made from the files their readers read
foreach(row "\tfsr_input_conventions\t0\tint\t0\t255\t" "1=Jitter reversed in x|2=Jitter reversed in y|"
            "|64=HDR color input\t\t\tboxes\n"
            "\tvulkan_ban_ignore\t\ttext\t" "VK_EXT_vertex_input_dynamic_state=VK_EXT_vertex_input_dynamic_state~nvidia < 580.119.02 breaks dynamic vertex input"
            "\thooks_off\t\ttext\t" "libc=libc~|libc_str=libc_str~the C library's string, character, number"
            "\thooks_verify\t\ttext\t"
            "\tvk_debug_hide_commands\t\ttext\t" "|vkCmdDrawMeshTasksEXT=vkCmdDrawMeshTasksEXT~Of VK_EXT_mesh_shader")
    string(FIND "${list}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings has no \"${row}\"")
    endif()
endforeach()
# a game's HDR output, off until asked for, and the paper white and the peak it is told: the launcher's Settings and
# a game's menu show them (gpu/vk/hdr_swapchain.c, hle/services/gsv.c)
foreach(row "Graphics\thdr_game\t0\tint\t0\t1\t" "\tGame HDR output\t0=Off|1=On\t\n"
            "Graphics\thdr_paperwhite\t0\tint\t0\t480\t" "\tPaper white (nits)\t0=Reference|10=10 nits|"
            "|480=480 nits\t\t\topen\n"
            "Graphics\thdr_peak\t1000\tint\t400\t4000\t" "\tPeak light (nits)\t400=400 nits|500=500 nits|"
            "|4000=4000 nits\t\t\topen\n"
            # the save states' slots and the upscaler's sharpness, sliders
            "\tSave state slots\t1=1|2=2|3=3|4=4|5=5|6=6|7=7|8=8|9=9|10=10|"
            "Graphics\tupscaler_sharpness\t75\tint\t0\t100\t" "\tUpscaler sharpness\t0=0%|5=5%|")
    string(FIND "${list}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings has no \"${row}\"")
    endif()
endforeach()
# the recompile's, the launcher's Launch group: the AOT threads' common counts, open as every row with steps
execute_process(COMMAND "${RUNTIME}" --settings=recompile RESULT_VARIABLE result OUTPUT_VARIABLE recompile
                ERROR_VARIABLE errors)
if(result OR NOT errors STREQUAL "")
    message(FATAL_ERROR "scarletnx --settings=recompile ended with ${result}: ${errors}")
endif()
foreach(row "\tAOT threads\t0=Auto|1=1|2=2|3=3|4=4|" "|256=256\t\t\topen\n" "\tUnit size\t5000=5000 instructions|")
    string(FIND "${recompile}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings=recompile has no \"${row}\"")
    endif()
endforeach()
