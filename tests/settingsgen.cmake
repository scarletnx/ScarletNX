# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# tools/settingsgen on a spec of its own, with LLVM headers written here: every setter of a bool or an integer is
# found whichever way its declaration is broken over lines (LLVM 20's headers put the name on the line after
# LLVM_C_ABI void, LLVM 19's after void on the same line), the others are left out, the passes of a pipeline get
# their toggles, and an @ID that LLVM lacks is said on stderr; every key with an _ is listed with a - for each, as a
# command line names it. Run by ctest as settingsgen, with SETTINGSGEN the generator and WORK a folder of its own
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out" "${WORK}/include/llvm-c/Transforms")
# a setting's name and an LLVM ID longer than any fixed buffer: 1100 letters (a line of generated C over 1024), and 126
# whose words and snake_case pass 192
string(REPEAT "Abcdefghij" 110 long_name)
string(REPEAT "Ab" 63 long_id)
file(WRITE "${WORK}/include/llvm-c/Transforms/PassBuilder.h" "
LLVM_C_ABI LLVMErrorRef LLVMRunPassesOnFunction(
    LLVMValueRef F, const char *Passes, LLVMTargetMachineRef TM,
    LLVMPassBuilderOptionsRef Options);
LLVM_C_ABI void
LLVMPassBuilderOptionsSetAAPipeline(LLVMPassBuilderOptionsRef Options,
                                    const char *AAPipeline);
LLVM_C_ABI void
LLVMPassBuilderOptionsSetLoopVectorization(LLVMPassBuilderOptionsRef Options,
                                           LLVMBool LoopVectorization);
void LLVMPassBuilderOptionsSetSLPVectorization(LLVMPassBuilderOptionsRef Options, LLVMBool SLPVectorization);
LLVM_C_ABI void LLVMPassBuilderOptionsSetForgetAllSCEVInLoopUnroll(
    LLVMPassBuilderOptionsRef Options, LLVMBool ForgetAllSCEVInLoopUnroll);
LLVM_C_ABI void
LLVMPassBuilderOptionsSetLicmMssaOptCap(LLVMPassBuilderOptionsRef Options,
                                        unsigned LicmMssaOptCap);
LLVM_C_ABI void
LLVMPassBuilderOptionsSetInlinerThreshold(LLVMPassBuilderOptionsRef Options,
                                          int Threshold);
/* avoid LLVMPassBuilderOptionsSetDebugLogging: a mention of a setter is not its declaration */
LLVM_C_ABI void LLVMPassBuilderOptionsSet${long_id}(LLVMPassBuilderOptionsRef Options, unsigned Cap);
")
file(WRITE "${WORK}/include/llvm-c/TargetMachine.h" "
LLVM_C_ABI void LLVMSetTargetMachineAsmVerbosity(LLVMTargetMachineRef T,
                                                 LLVMBool VerboseAsm);
LLVM_C_ABI void
LLVMSetTargetMachineGlobalISelAbort(LLVMTargetMachineRef T,
                                    LLVMGlobalISelAbortMode Mode);
LLVM_C_ABI void LLVMSetTargetMachineFastISel(LLVMTargetMachineRef T,
                                             LLVMBool Enable);
")
file(WRITE "${WORK}/pipelines.txt"
     "0 function(ee-instrument<>),always-inline,verify\n2 function<eager-inv>(sroa<modify-cfg>,instcombine<max-iterations=1;no-verify-fixpoint>),require<globals-aa>,verify\n")
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
setting Loudness
    key loudness
    int 0 100
    default 100
    words Mute|#%
    step 5
    slider
setting ${long_name}
    key long
    text 4
    help ${long_name}
table g_test_llvm_ini llvm
setting @instcombine
    name Instruction combining
    help Combines instructions (instcombine)
setting @LoopVectorization
    name Loop vectorization
    help Whether the pipeline vectorizes loops
setting @InlinerThreshold
    name Inliner threshold
    help How large a function the inliner inlines
setting @NoSuchOption
    name Not in this LLVM
    help An ID this LLVM lacks, said and left out
setting @FastISel
    key llvm_fast_isel
table g_test_vk_ini vulkan
setting @VK_EXT_core_one
setting @VK_KHR_own_help
    help Its own help first
section Test: g_test_ini, g_test_vk_ini/Vulkan Extensions
section Launch: g_test_llvm_ini/LLVM Internals    launcher
")
# the Khronos registry as far as the vulkan table reads it: each extension's tag
file(WRITE "${WORK}/vk.xml" "<registry><extensions>
<extension name=\"VK_EXT_core_one\" number=\"7\" type=\"device\" promotedto=\"VK_VERSION_1_3\" supported=\"vulkan,vulkansc\">
</extension>
<extension name=\"VK_KHR_own_help\" number=\"8\" type=\"device\" deprecatedby=\"VK_KHR_newer\" supported=\"vulkan\"></extension>
<extension name=\"VK_KHR_instance_one\" number=\"9\" type=\"instance\" supported=\"vulkan\"></extension>
</extensions></registry>
")
execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" --pipelines "${WORK}/pipelines.txt"
                        --llvm-include "${WORK}/include" --vk-registry "${WORK}/vk.xml" "${WORK}/settings.txt"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result)
    message(FATAL_ERROR "settingsgen ended with ${result}: ${errors}")
endif()
# an @ID this LLVM lacks is said, with its line, and is the only thing said
if(NOT errors MATCHES "^[^\n]*settings.txt:[0-9]+: @NoSuchOption is no pass of the pipelines nor an option of this LLVM's headers: left out\n$")
    message(FATAL_ERROR "settingsgen did not say @NoSuchOption is left out, or said more:\n${errors}")
endif()
file(READ "${WORK}/out/settings_llvm.c" llvm)
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
            "{\"plain\", \"0\", NULL, INI_INT, 0, &g_settings.plain,"
            # a slider's (ini.h's INI_SLIDER): an int with words, the menus' steps
            "{\"loudness\", \"100\", NULL, INI_INT, INI_SLIDER, &g_settings.loudness, 0, 100, NULL, \"Loudness\", \"Mute|#%\", 5, 0,")
    string(FIND "${text}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_test.c lacks the row ${row}:\n${text}")
    endif()
endforeach()
# a system only does not know is refused with its line, the word and the systems it takes
file(WRITE "${WORK}/unknown.txt" "table g_test_ini\nsetting Elsewhere\n    key elsewhere\n    int 0 1\n    default 0\n    only amiga\n")
execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" "${WORK}/unknown.txt"
                RESULT_VARIABLE unknown_result OUTPUT_VARIABLE unknown_output ERROR_VARIABLE unknown_errors)
if(NOT unknown_result OR NOT unknown_errors MATCHES "unknown.txt:6: only amiga: the systems are macos, windows, android, linux, arm64-linux and none\n$")
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
# each setter found, with its argument cast to the type the header declares, and the setting its @ID names
foreach(line "LLVMPassBuilderOptionsSetLoopVectorization(options, (LLVMBool)s->loop_vectorization)"
             "LLVMPassBuilderOptionsSetSLPVectorization(options, (LLVMBool)s->llvm_slp_vectorization)"
             "LLVMPassBuilderOptionsSetForgetAllSCEVInLoopUnroll(options, (LLVMBool)s->llvm_forget_all_scev_in_loop_unroll)"
             "LLVMPassBuilderOptionsSetLicmMssaOptCap(options, (unsigned)s->llvm_licm_mssa_opt_cap)"
             "LLVMPassBuilderOptionsSetInlinerThreshold(options, (int)s->inliner_threshold)"
             "LLVMSetTargetMachineAsmVerbosity(machine, (LLVMBool)s->llvm_asm_verbosity)"
             "LLVMSetTargetMachineFastISel(machine, (LLVMBool)s->llvm_fast_i_sel)")
    string(FIND "${llvm}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_llvm.c lacks \"${line}\":\n${llvm}")
    endif()
endforeach()
# a setter of a string or an enum, a function that is no setter, and a setter only mentioned: none
foreach(word AAPipeline GlobalISelAbort RunPassesOnFunction DebugLogging)
    if(llvm MATCHES "${word}")
        message(FATAL_ERROR "settings_llvm.c sets ${word}, which takes no bool or integer:\n${llvm}")
    endif()
endforeach()
# the pipelines' passes, named as they are, a toggle each: not a nested pipeline (function), nor require<>
foreach(pass instcombine sroa verify always-inline ee-instrument)
    string(FIND "${llvm}" "{\"${pass}\", offsetof(Settings, " at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_llvm.c has no toggle for the pass ${pass}:\n${llvm}")
    endif()
endforeach()
if(llvm MATCHES "\"function\", offsetof|\"require\", offsetof")
    message(FATAL_ERROR "settings_llvm.c toggles a pipeline or a require<>:\n${llvm}")
endif()
# the members: the @ID's name in snake_case, else LLVM's; an int option ranges to INT_MAX, a bool to 1
foreach(member "int instruction_combining; /* llvm_pass_instcombine */" "int llvm_pass_sroa; /* llvm_pass_sroa */"
               "int inliner_threshold; /* llvm_inliner_threshold */" "int llvm_slp_vectorization; /* llvm_slp_vectorization */")
    string(FIND "${header}" "${member}" at)
    if(at LESS 0)
        message(FATAL_ERROR "snx_settings.h lacks \"${member}\":\n${header}")
    endif()
endforeach()
file(READ "${WORK}/out/settings_launch.c" launch)
file(READ "${WORK}/out/settings_test.c" runtime)
foreach(row "{\"llvm_inliner_threshold\", NULL, \"How large a function the inliner inlines\", INI_INT, 0, &g_settings.inliner_threshold, INT_MIN + 1, INT_MAX, NULL, \"Inliner threshold\", NULL, 0, 0, 0}"
            "{\"llvm_slp_vectorization\", NULL, \"LLVMPassBuilderOptionsSetSLPVectorization, LLVM's own value until it is set\", INI_INT, 0, &g_settings.llvm_slp_vectorization, 0, 1, NULL, \"LLVM SLP vectorization\", \"Off|On\", 0, 0, 0}"
            # the key an @ID gives, where snake_case of the ID would not do (fast_i_sel); its name LLVM's still
            "{\"llvm_fast_isel\", NULL, \"LLVMSetTargetMachineFastISel, LLVM's own value until it is set\", INI_INT, 0, &g_settings.llvm_fast_i_sel, 0, 1, NULL, \"LLVM fast i sel\", \"Off|On\", 0, 0, 0}")
    string(FIND "${launch}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_launch.c lacks the row ${row}:\n${launch}")
    endif()
endforeach()
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
# the long ones, whole: the member of the 1100-letter name (its words joined by _), the long setter's key and name
string(REPEAT "abcdefghij_" 110 long_member)
string(REGEX REPLACE "_$" "" long_member "${long_member}")
string(REPEAT "ab_" 63 long_key)
string(REGEX REPLACE "_$" "" long_key "${long_key}")
string(REPEAT "ab " 63 long_words)
string(REGEX REPLACE " $" "" long_words "${long_words}")
foreach(line "char ${long_member}[4]; /* long */"
             "int llvm_${long_key}; /* llvm_${long_key} */")
    string(FIND "${header}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "snx_settings.h lacks the long \"${line}\":\n${header}")
    endif()
endforeach()
foreach(line "LLVMPassBuilderOptionsSet${long_id}(options, (unsigned)s->llvm_${long_key})")
    string(FIND "${llvm}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "settings_llvm.c lacks the long \"${line}\":\n${llvm}")
    endif()
endforeach()
foreach(line "{\"llvm_${long_key}\", NULL, \"LLVMPassBuilderOptionsSet${long_id}, LLVM's own value until it is set\", INI_INT, 0, &g_settings.llvm_${long_key}, 0, INT_MAX, NULL, \"LLVM ${long_words}\", NULL, 0, 0, 0}"
             "{\"long\", \"\", \"${long_name}\", INI_TEXT, 0, g_settings.${long_member}, 0, 0, NULL, \"${long_name}\", NULL, 0, 0, sizeof g_settings.${long_member}}")
    string(FIND "${launch};${runtime}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "the tables lack the long row ${line}:\n${launch}\n${runtime}")
    endif()
endforeach()
if(header MATCHES "no_such_option|not_in_this_llvm")
    message(FATAL_ERROR "snx_settings.h has a member for @NoSuchOption, which this LLVM lacks:\n${header}")
endif()

# the vulkan table: each extension a text setting keyed by its name in lowercase, auto by default, its help from the
# registry after its own, and listed for the renderer with the version it is core in
file(READ "${WORK}/out/settings_test_vk.c" vk)
foreach(line "{\"vk_ext_core_one\", \"auto\", \"The Vulkan extension VK_EXT_core_one, the Khronos registry's number 7, promoted to Vulkan 1.3.\\nauto = "
             "{\"vk_khr_own_help\", \"auto\", \"Its own help first\\nThe Vulkan extension VK_KHR_own_help, the Khronos registry's number 8, deprecated by VK_KHR_newer.\\n"
             "INI_TEXT, INI_STARTUP, g_settings.vk_ext_core_one, 0, 0, NULL, \"VK_EXT_core_one\", \"auto=Auto|on=On|off=Off\""
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
# each's once per row, LLVM's and the Vulkan extensions' too, none of a key without an _), in the order strcmp sorts
# them, which ini_arg's search takes: two-hosts before two1-more, whose keys sort the other way (_ after 1)
string(REPEAT "ab-" 63 long_arg)
string(REGEX REPLACE "-$" "" long_arg "${long_arg}")
set(names "")
foreach(name its-kernel llvm-${long_arg} llvm-asm-verbosity llvm-fast-isel llvm-forget-all-scev-in-loop-unroll
             llvm-inliner-threshold llvm-licm-mssa-opt-cap llvm-loop-vectorization llvm-pass-always-inline
             llvm-pass-ee-instrument llvm-pass-instcombine llvm-pass-sroa llvm-pass-verify llvm-slp-vectorization
             one-system pad1-swap pad2-swap side-l side-stick-r two-hosts two1-more vk-ext-core-one vk-khr-own-help)
    string(APPEND names "    \"${name}\",\n")
endforeach()
file(READ "${WORK}/out/settings_args.c" args)
string(FIND "${args}" "const char* const g_setting_args[] = {\n${names}};\nconst int g_setting_args_count = 23;\n" at)
if(at LESS 0)
    message(FATAL_ERROR "settings_args.c does not list these 23 names, in this order:\n${names}\nbut:\n${args}")
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
            "list sides a-b=A\ntable g_t_ini\nsetting Side\n    key side_<ID>\n    int 0 1\n    default 0\n    each sides\n|settingsgen: list sides: its id a-b has a -, which its settings' keys would have: an id is letters, digits and _")
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
