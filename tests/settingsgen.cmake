# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# tools/settingsgen on a spec of its own: every key with an _ is listed with a - for each, as a command line names it. Run by ctest as settingsgen, with SETTINGSGEN the generator and WORK a folder of its own
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out")
# a setting's name longer than any fixed buffer: 1100 letters (a line of generated C over 1024)
string(REPEAT "Abcdefghij" 110 long_name)
file(WRITE "${WORK}/settings.txt" "
list sides l=Left stick_r=Right_stick
table g_test_ini
setting Plain
    key plain
    int 0 1
    default 0
setting Both
    key both
    int 0 1
    default 1
    recompile
setting Two hosts
    key two_hosts
    int 0 9
    default 1
    default-if HOST_A 2
    default-if HOST_B 3
setting Two more
    key two1_more
    int 0 1
    default 0
setting Side
    key side_<ID>
    int 0 1
    default 0
    each sides
repeat 2
setting Pad swap
    key pad<N>_swap
    int 0 1
    default 0
end
setting One system
    key one_system
    int 0 1
    default 0
    startup
    only macos
setting Its kernel
    key its_kernel
    int 0 1
    default 0
    only arm64-linux
setting Both Apples
    key apples
    int 0 1
    default 0
    only apple
setting Loudness
    key loudness
    int 0 100
    default 100
    words Mute|#%
    step 5
    slider
setting Speed
    key speed
    int 0 9
    default 1
    startup
    perf
setting Driver
    key driver
    text 16
    once
    perf
setting Corpus
    key corpus
    text 16
    path folder
setting Profile
    key profile
    text 16
    path file
    startup
setting Stats
    key stats
    text 16
    path save
setting ${long_name}
    key long
    text 4
    help ${long_name}
table g_test_vk_ini vulkan
setting @VK_EXT_core_one
setting @VK_KHR_own_help
    help Its own help first
section Test: g_test_ini, g_test_vk_ini/Vulkan Extensions
")
# the Khronos registry as far as the vulkan table reads it: each extension's tag
file(WRITE "${WORK}/vk.xml" "<registry><extensions>
<extension name=\"VK_EXT_core_one\" number=\"7\" type=\"device\" promotedto=\"VK_VERSION_1_3\" supported=\"vulkan,vulkansc\">
</extension>
<extension name=\"VK_KHR_own_help\" number=\"8\" type=\"device\" deprecatedby=\"VK_KHR_newer\" supported=\"vulkan\"></extension>
<extension name=\"VK_KHR_instance_one\" number=\"9\" type=\"instance\" supported=\"vulkan\"></extension>
</extensions></registry>
")
execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" --vk-registry "${WORK}/vk.xml" "${WORK}/settings.txt"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result)
    message(FATAL_ERROR "settingsgen ended with ${result}: ${errors}")
endif()
# nothing said
if(NOT errors STREQUAL "")
    message(FATAL_ERROR "settingsgen said:\n${errors}")
endif()
file(READ "${WORK}/out/snx_settings.h" header)
# a setting with two default-ifs: the first that holds, else its default, for g_settings and the table's row
file(READ "${WORK}/out/settings.c" values)
foreach(what INIT DEF)
    if(what STREQUAL INIT)
        set(text "${values}")
        set(a 2)
        set(b 3)
        set(c 1)
    else()
        file(READ "${WORK}/out/settings_test.c" text)
        set(a "\"2\"")
        set(b "\"3\"")
        set(c "\"1\"")
    endif()
    string(FIND "${text}" "#if defined(HOST_A)\n#define SETTING_${what}_two_hosts ${a}\n#elif defined(HOST_B)\n#define SETTING_${what}_two_hosts ${b}\n#else\n#define SETTING_${what}_two_hosts ${c}\n#endif" at)
    if(at LESS 0)
        message(FATAL_ERROR "SETTING_${what}_two_hosts is not HOST_A's, else HOST_B's, else the default:\n${text}")
    endif()
endforeach()
# a setting of one system alone: its row's flags name the system (ini.h's INI_ONLY_), after startup's; another row's
# name none
file(READ "${WORK}/out/settings_test.c" text)
foreach(row "{\"one_system\", \"0\", NULL, INI_INT, INI_STARTUP | INI_ONLY_MACOS, &g_settings.one_system,"
            "{\"its_kernel\", \"0\", NULL, INI_INT, INI_ONLY_ARM64_LINUX, &g_settings.its_kernel,"
            "{\"apples\", \"0\", NULL, INI_INT, INI_ONLY_APPLE, &g_settings.both_apples,"
            "{\"plain\", \"0\", NULL, INI_INT, 0, &g_settings.plain,"
            # a slider's (ini.h's INI_SLIDER): an int with words, the menus' steps
            "{\"loudness\", \"100\", NULL, INI_INT, INI_SLIDER, &g_settings.loudness, 0, 100, NULL, \"Loudness\", \"Mute|#%\", 5, 0,"
            # a perf row's (ini.h's INI_PERF), after startup's; a once text's with no default in its row
            "{\"speed\", \"1\", NULL, INI_INT, INI_STARTUP | INI_PERF, &g_settings.speed,"
            "{\"driver\", NULL, NULL, INI_TEXT, INI_PERF, g_settings.driver,"
            # a path's (ini.h's INI_PATH): a folder's, a file's, one to write
            "{\"corpus\", \"\", NULL, INI_TEXT, INI_PATH_FOLDER, g_settings.corpus,"
            "{\"profile\", \"\", NULL, INI_TEXT, INI_STARTUP | INI_PATH_FILE, g_settings.profile,"
            "{\"stats\", \"\", NULL, INI_TEXT, INI_PATH_SAVE, g_settings.stats,")
    string(FIND "${text}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_test.c lacks the row ${row}:\n${text}")
    endif()
endforeach()
# a system only does not know is refused with its line, the word and the systems it takes
file(WRITE "${WORK}/unknown.txt" "table g_test_ini\nsetting Elsewhere\n    key elsewhere\n    int 0 1\n    default 0\n    only amiga\n")
execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" "${WORK}/unknown.txt"
                RESULT_VARIABLE unknown_result OUTPUT_VARIABLE unknown_output ERROR_VARIABLE unknown_errors)
if(NOT unknown_result OR NOT unknown_errors MATCHES "unknown.txt:6: only amiga: the systems are macos, apple, windows, android, linux, arm64-linux and none\n$")
    message(FATAL_ERROR "settingsgen took only amiga (${unknown_result}), or did not say which systems only takes:\n${unknown_errors}")
endif()
# a slider of a text, or of an int without words, is refused with its line: its steps are an int's words
foreach(lines "text 8\n    default a" "int 0 9\n    default 0")
    file(WRITE "${WORK}/slider.txt" "table g_test_ini\nsetting Wordless\n    key wordless\n    ${lines}\n    slider\n")
    execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" "${WORK}/slider.txt"
                    RESULT_VARIABLE slider_result ERROR_VARIABLE slider_errors)
    if(NOT slider_result OR NOT slider_errors MATCHES "slider.txt:2: Wordless: slider takes an int with words, the steps its slider shows\n$")
        message(FATAL_ERROR "settingsgen took a slider of ${lines} (${slider_result}), or did not say why:\n${slider_errors}")
    endif()
endforeach()
# steps that do not go up, are no whole numbers or leave the range, steps on a text, and open on an int, refused with
# their line and why
foreach(case "int 0 9\n    default 0\n    steps 0 4 2@@steps.txt:6: steps go up: 2 after 4"
             "int 0 9\n    default 0\n    steps 0 x@@steps.txt:6: steps takes whole numbers: x"
             "int 0 9\n    default 0\n    steps 0 12@@steps.txt:2: Stepped: step 12 is outside 0..9"
             "int 0 90\n    default 0\n    steps 30..10/10@@steps.txt:6: steps: a range goes up: 30..10/10"
             "int 0 90\n    default 0\n    steps 0..20/5 20@@steps.txt:6: steps go up: 20 after 20"
             "int 0 90\n    default 0\n    steps 0..20/x@@steps.txt:6: steps: a range is A..B/S: 0..20/x"
             "text 8\n    default a\n    steps 1 2@@steps.txt:2: Stepped: steps takes an int, not a slider"
             "int 0 9\n    default 0\n    words Off|On\n    open@@steps.txt:2: Stepped: open takes a text with words")
    string(REPLACE "@@" ";" parts "${case}")
    list(GET parts 0 lines)
    list(GET parts 1 said)
    file(WRITE "${WORK}/steps.txt" "table g_test_ini\nsetting Stepped\n    key stepped\n    ${lines}\n")
    execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" "${WORK}/steps.txt"
                    RESULT_VARIABLE steps_result ERROR_VARIABLE steps_errors)
    string(FIND "${steps_errors}" "${said}" at)
    if(NOT steps_result OR at LESS 0)
        message(FATAL_ERROR "settingsgen took \"${lines}\" (${steps_result}), or did not say \"${said}\":\n${steps_errors}")
    endif()
endforeach()
file(READ "${WORK}/out/settings_launch.c" launch)
file(READ "${WORK}/out/settings_test.c" runtime)
# a game's setting the recompile takes too: its row in the game's table and in the recompiler's file's own
foreach(file launch runtime)
    string(FIND "${${file}}" "{\"both\", \"1\", NULL, INI_INT, 0, &g_settings.both, 0, 1, NULL, \"Both\"" at)
    if(at LESS 0)
        message(FATAL_ERROR "the ${file} file lacks the row of both:\n${${file}}")
    endif()
endforeach()
string(FIND "${launch}" "const IniOpt g_recompile_game_ini[] = {\n    {\"both\"" at)
if(at LESS 0)
    message(FATAL_ERROR "settings_launch.c has no g_recompile_game_ini with both:\n${launch}")
endif()
# the long one, whole: the member of the 1100-letter name (its words joined by _)
string(REPEAT "abcdefghij_" 110 long_member)
string(REGEX REPLACE "_$" "" long_member "${long_member}")
foreach(line "char ${long_member}[4]; /* long */")
    string(FIND "${header}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "snx_settings.h lacks the long \"${line}\":\n${header}")
    endif()
endforeach()
foreach(line "{\"long\", \"\", \"${long_name}\", INI_TEXT, 0, g_settings.${long_member}, 0, 0, NULL, \"${long_name}\", NULL, 0, 0, sizeof g_settings.${long_member}, NULL}")
    string(FIND "${launch};${runtime}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "the tables lack the long row ${line}:\n${launch}\n${runtime}")
    endif()
endforeach()

# the vulkan table: each extension a text setting keyed by its name in lowercase, auto by default, its help from the
# registry after its own, and listed for the renderer with the version it is core in
file(READ "${WORK}/out/settings_test_vk.c" vk)
foreach(line "{\"vk_ext_core_one\", \"auto\", \"The Vulkan extension VK_EXT_core_one, the Khronos registry's number 7, promoted to Vulkan 1.3.\\nauto = "
             "{\"vk_khr_own_help\", \"auto\", \"Its own help first\\nThe Vulkan extension VK_KHR_own_help, the Khronos registry's number 8, deprecated by VK_KHR_newer.\\n"
             "INI_TEXT, INI_STARTUP | INI_PERF, g_settings.vk_ext_core_one, 0, 0, NULL, \"VK_EXT_core_one\", \"auto=Auto|on=On|off=Off\""
             "{\"VK_EXT_core_one\", g_settings.vk_ext_core_one, 13},"
             "{\"VK_KHR_own_help\", g_settings.vk_khr_own_help, 0},"
             "const int g_vk_extension_settings_count = 2;")
    string(FIND "${vk}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_test_vk.c lacks ${line}:\n${vk}")
    endif()
endforeach()
# a name the registry has not, and an instance's extension, are refused with their line
foreach(bad "VK_EXT_no_such_one|is no extension of the Khronos registry" "VK_KHR_instance_one|not a device's")
    string(REPLACE "|" ";" bad "${bad}")
    list(GET bad 0 name)
    list(GET bad 1 why)
    file(WRITE "${WORK}/bad.txt" "table g_bad_ini vulkan\nsetting @${name}\n")
    execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" --vk-registry "${WORK}/vk.xml" "${WORK}/bad.txt"
                    RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result OR NOT errors MATCHES "bad.txt:2: ${name} [^\n]*${why}")
        message(FATAL_ERROR "settingsgen took @${name} (${result}): ${errors}")
    endif()
endforeach()

# the names a command line gives the settings by: every row's key that has an _, with a - for each (a repeat's and an
# each's once per row, the Vulkan extensions' too, none of a key without an _), in the order strcmp sorts them, which
# ini_arg's search takes: two-hosts before two1-more, whose keys sort the other way (_ after 1)
set(names "")
foreach(name its-kernel one-system pad1-swap pad2-swap side-l side-stick-r two-hosts two1-more vk-ext-core-one
             vk-khr-own-help)
    string(APPEND names "    \"${name}\",\n")
endforeach()
file(READ "${WORK}/out/settings_args.c" args)
string(FIND "${args}" "const char* const g_setting_args[] = {\n${names}};\nconst int g_setting_args_count = 10;\n" at)
if(at LESS 0)
    message(FATAL_ERROR "settings_args.c does not list these 10 names, in this order:\n${names}\nbut:\n${args}")
endif()
string(FIND "${args}" "return ini_arg(arg, key, size, g_setting_args, g_setting_args_count);" at)
string(FIND "${header}" "const char* settings_arg(const char* arg, char* key, size_t size);" declared)
if(at LESS 0 OR declared LESS 0)
    message(FATAL_ERROR "settings_arg is not declared, or not ini_arg over the names:\n${header}\n${args}")
endif()
# a key a command line could not name as it is, or whose name with - could be another key's, is refused with its line
# before anything is written: a - or a stray <N> in a key, a - in an each's id
foreach(bad "table g_t_ini\nsetting Dashed\n    key a-b\n    int 0 1\n    default 0\n|refused.txt:2: Dashed's key a-b has a -: a key is letters, digits and _ (a command line gives it with - for each _ too)"
            "table g_t_ini\nsetting Stray\n    key a<N>\n    int 0 1\n    default 0\n|refused.txt:2: Stray's key a<N> has a <: a key is letters, digits and _ (a command line gives it with - for each _ too)"
            "list sides a-b=A\ntable g_t_ini\nsetting Side\n    key side_<ID>\n    int 0 1\n    default 0\n    each sides\n|settingsgen: list sides: its id a-b has a -, which its settings' keys would have: an id is letters, digits and _"
            # path on a row that is no path: an int's, and a kind no picker has
            "table g_t_ini\nsetting Number\n    key number\n    int 0 1\n    default 0\n    path file\n|refused.txt:2: Number: path takes a text, not boxes"
            "table g_t_ini\nsetting Odd\n    key odd\n    text 8\n    default a\n    path banana\n|refused.txt:6: path banana: a path is a folder, a file, or save (a file to write)"
            # perf on a row the game never reads, or on a once row whose off-default nothing can tell
            "table g_l_ini launcher\nsetting Shown\n    key shown\n    int 0 1\n    default 0\n    perf\n|refused.txt:2: Shown: perf takes a game's or the recompile's setting: a launcher table's never reaches the game"
            "table g_t_ini\nsetting Every\n    key every\n    int 0 9\n    default 0\n    once\n    perf\n|refused.txt:2: Every: perf takes a once setting only as a text whose default is empty")
    string(REPLACE "|" ";" bad "${bad}")
    list(GET bad 0 spec)
    list(GET bad 1 why)
    file(REMOVE_RECURSE "${WORK}/refused")
    file(MAKE_DIRECTORY "${WORK}/refused")
    file(WRITE "${WORK}/refused.txt" "${spec}")
    execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/refused" "${WORK}/refused.txt"
                    RESULT_VARIABLE result ERROR_VARIABLE errors)
    file(GLOB written "${WORK}/refused/*")
    string(REPLACE "${WORK}/" "" said "${errors}")
    if(NOT result OR NOT said STREQUAL "${why}\n" OR written)
        message(FATAL_ERROR "settingsgen took or wrote it (${result}, ${written}), or did not say \"${why}\":\n${spec}\n${errors}")
    endif()
endforeach()
# a row of boxes (settings.txt's boxes): its items in its row's words, value=Label~About between |, and INI_BOXES in its
# flags; from the bans' file (a key once, the lines on it as its about), from the hook families' (the comment before one
# its about), from the registry's device commands (an alias's as its command's, a physical device's and a Vulkan SC
# block's left out) and from the row's own words. Without its file a row of boxes is typed: no words, no INI_BOXES
file(WRITE "${WORK}/boxes.txt" "table g_boxes_ini
setting Bans
    key bans
    text 64
    default \"\"
    boxes bans
setting Families
    key families
    text 64
    default \"\"
    boxes families
setting Commands
    key commands
    text 64
    default \"\"
    boxes commands
setting Bits
    key bits
    int 0 255
    default 0
    words 1=One|2=Two|64=Six
    boxes words
table g_boxes_vk_ini vulkan
setting @VK_EXT_core_one
setting @VK_KHR_own_help
section Test: g_boxes_ini, g_boxes_vk_ini/Vulkan Extensions
")
file(WRITE "${WORK}/bans.txt" "# a comment
key | action | driver | versions | system | devices | breaks | source | retest | checked | override

BAN_ONE | off | nvidia | < 5.1 | any | * | breaks a thing | here | none | never | -
BAN_TWO | warn | mali | * | android | * | tears | there | none | never | -
BAN_ONE | prefer | radv | * | any | * | is faster ~ there | here | none | never | -
")
file(WRITE "${WORK}/hooks.txt" "# the file's head

route a
# one family's words
# in two lines
family one
hook h(u64 a) -> u64 = x
family two
")
file(WRITE "${WORK}/boxes-vk.xml" "<registry>
<commands>
<command><proto><type>void</type> <name>vkCmdA</name></proto><param><type>VkCommandBuffer</type> <name>c</name></param></command>
<command><proto><type>void</type> <name>vkGetPhys</name></proto><param><type>VkPhysicalDevice</type> <name>p</name></param></command>
<command name=\"vkAliasKHR\" alias=\"vkCmdA\"/>
<command><proto><type>void</type> <name>vkCmdSC</name></proto><param><type>VkCommandBuffer</type> <name>c</name></param></command>
</commands>
<extensions>
<extension name=\"VK_EXT_core_one\" number=\"7\" type=\"device\" supported=\"vulkan\">
<require><command name=\"vkCmdA\"/><command name=\"vkGetPhys\"/></require>
<require api=\"vulkansc\"><command name=\"vkCmdSC\"/></require>
</extension>
<extension name=\"VK_KHR_own_help\" number=\"8\" type=\"device\" supported=\"vulkan\">
<require><command name=\"vkAliasKHR\"/><command name=\"vkCmdA\"/></require>
</extension>
</extensions></registry>
")
file(MAKE_DIRECTORY "${WORK}/boxes" "${WORK}/typed")
execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/boxes" --vk-registry "${WORK}/boxes-vk.xml"
                        --bans "${WORK}/bans.txt" --hooks "${WORK}/hooks.txt" "${WORK}/boxes.txt"
                RESULT_VARIABLE result ERROR_VARIABLE errors)
if(result OR NOT errors STREQUAL "")
    message(FATAL_ERROR "settingsgen of boxes ended with ${result}: ${errors}")
endif()
file(READ "${WORK}/boxes/settings_boxes.c" text)
foreach(row "{\"bans\", \"\", NULL, INI_TEXT, INI_BOXES, g_settings.bans, 0, 0, NULL, \"Bans\", \"BAN_ONE=BAN_ONE~nvidia < 5.1 breaks a thing; radv is faster - there|BAN_TWO=BAN_TWO~mali (android) tears\", "
            "{\"families\", \"\", NULL, INI_TEXT, INI_BOXES, g_settings.families, 0, 0, NULL, \"Families\", \"one=one~one family's words in two lines|two=two~\", "
            "{\"commands\", \"\", NULL, INI_TEXT, INI_BOXES, g_settings.commands, 0, 0, NULL, \"Commands\", \"vkCmdA=vkCmdA~Of VK_EXT_core_one, VK_KHR_own_help|vkAliasKHR=vkAliasKHR~Of VK_KHR_own_help\", "
            "{\"bits\", \"0\", NULL, INI_INT, INI_BOXES, &g_settings.bits, 0, 255, NULL, \"Bits\", \"1=One|2=Two|64=Six\", ")
    string(FIND "${text}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_boxes.c lacks the row ${row}:\n${text}")
    endif()
endforeach()
execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/typed" "${WORK}/boxes.txt" RESULT_VARIABLE result ERROR_VARIABLE errors)
if(result)
    message(FATAL_ERROR "settingsgen of boxes without their files ended with ${result}: ${errors}")
endif()
file(READ "${WORK}/typed/settings_boxes.c" text)
string(REGEX MATCHALL "INI_BOXES" typed "${text}")
list(LENGTH typed typed_count)
string(FIND "${text}" "{\"bans\", \"\", NULL, INI_TEXT, 0, g_settings.bans, 0, 0, NULL, \"Bans\", NULL, " at)
if(NOT typed_count EQUAL 1 OR at LESS 0) # the bits' words are its own
    message(FATAL_ERROR "a row of boxes whose file was not given is not typed (${typed_count} INI_BOXES):\n${text}")
endif()
# a source settingsgen does not know, boxes of a float, with steps, and of words with none or none an int's bit, refused
# with their line
foreach(case "text 8\n    default a\n    boxes amiga@@boxes.txt:6: boxes amiga: the items are words, bans, families or commands"
             "float 0 1\n    default 0\n    boxes words@@boxes.txt:2: Boxed: boxes takes an int or a text, a float is no set"
             "int 0 9\n    default 0\n    boxes words@@boxes.txt:2: Boxed: boxes words takes words, value=Label each"
             "int 0 9\n    default 0\n    words Off|On\n    boxes words@@boxes.txt:2: Boxed: boxes words takes value=Label each: Off"
             "int 0 9\n    default 0\n    words 3=Three\n    boxes words@@boxes.txt:2: Boxed: an int's box is one bit of its range: 3"
             "int 0 9\n    default 0\n    words 16=Sixteen\n    boxes words@@boxes.txt:2: Boxed: an int's box is one bit of its range: 16"
             "text 8\n    default a\n    words a=A\n    boxes bans@@boxes.txt:2: Boxed: boxes bans takes a text without words"
             "int 0 9\n    default 0\n    steps 0 4\n    boxes words@@boxes.txt:2: Boxed: boxes takes no slider, steps or open")
    string(REPLACE "@@" ";" parts "${case}")
    list(GET parts 0 lines)
    list(GET parts 1 said)
    file(WRITE "${WORK}/boxes-bad.txt" "table g_test_ini\nsetting Boxed\n    key boxed\n    ${lines}\n")
    execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" "${WORK}/boxes-bad.txt"
                    RESULT_VARIABLE boxes_result ERROR_VARIABLE boxes_errors)
    string(REPLACE "boxes.txt" "boxes-bad.txt" said "${said}")
    string(FIND "${boxes_errors}" "${said}" at)
    if(NOT boxes_result OR at LESS 0)
        message(FATAL_ERROR "settingsgen took \"${lines}\" (${boxes_result}), or did not say \"${said}\":\n${boxes_errors}")
    endif()
endforeach()
