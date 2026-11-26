/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx-vk-replay RECORD: the graphics pipelines of a pipeline record (pipeline_records=DIR, gpu/vk/pipeline_record.h)
   made again away from the game, to take a driver's failure or crash apart. Exit 0 when the driver made them, 3 when
   it answered an error, 1 when the record does not read, 2 for the command line */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/common.h"
#include "nx/gpu/vk/pipeline_record.h"

/* where a driver's crash landed, in the crash report's form: the module, and the offset a debugger takes */
#if defined(_WIN32)
static LONG WINAPI replay_crashed(EXCEPTION_POINTERS* e) {
    void* pc = e->ExceptionRecord->ExceptionAddress;
    HMODULE m = NULL;
    char name[MAX_PATH] = "an unknown module";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &m))
        GetModuleFileNameA(m, name, sizeof name);
    fprintf(stderr, "snx-vk-replay: crashed (exception %#lx) in %s +%llx\n",
            (unsigned long)e->ExceptionRecord->ExceptionCode, snx_path_leaf(name),
            (unsigned long long)((uintptr_t)pc - (uintptr_t)m));
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void replay_crashed(int sig) {
    static const char line[] = "snx-vk-replay: crashed (a signal) while the driver made the record's objects\n";
    (void)!write(2, line, sizeof line - 1);
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

int main(int argc, char** argv) {
    static const char usage[] =
        "usage: snx-vk-replay RECORD\n"
        "  makes again the graphics pipelines of the record in the folder RECORD (pipeline_records=DIR keeps each\n"
        "  in DIR/<run>-<n>), on a device made as its device.txt says\n";
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        fputs(usage, stdout);
        return 0;
    }
    if (argc != 2 || argv[1][0] == '-') {
        if (argc == 2) fprintf(stderr, "snx-vk-replay: unknown option: %s\n", argv[1]);
        fputs(usage, stderr);
        return 2;
    }
    if (!snx_is_dir(argv[1])) {
        fprintf(stderr, "snx-vk-replay: %s is no folder\n", argv[1]);
        return 1;
    }
#if defined(_WIN32)
    SetUnhandledExceptionFilter(replay_crashed);
#else
    signal(SIGSEGV, replay_crashed);
    signal(SIGBUS, replay_crashed);
    signal(SIGILL, replay_crashed);
#endif
    VkResult made = VK_SUCCESS;
    char why[1024];
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "snx-vk-replay: no Vulkan loader on this system\n");
        return 1;
    }
    if (vk_pipeline_replay(VK_NULL_HANDLE, argv[1], &made, why, sizeof why)) {
        fprintf(stderr, "snx-vk-replay: %s: %s\n", argv[1], why);
        return 1;
    }
    printf("snx-vk-replay: %s: the driver answered %s (%d)\n", argv[1], snx_vk_result_str(made), (int)made);
    return made >= 0 ? 0 : 3;
}
