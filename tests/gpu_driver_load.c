/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nx/runtime/platform/gpu_driver_load.c away from Android: Freedreno's variables set before any driver reads them, and
   each run that asks for what it cannot have stopped, saying why, rather than going on with the system's driver. A run
   opens its driver once, so each case is a run of this program: env, badenv, here, badid */
#include "nx/runtime/platform/gpu_driver_load.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h" /* fatal and logf_, which this file stands in for */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    jmp_buf stopped;  /* where fatal goes back to */
    char fatal[1024]; /* what it said */
    char log[4096];   /* what the run logged */
} g_test;
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_test.fatal, sizeof g_test.fatal, fmt, ap);
    va_end(ap);
    longjmp(g_test.stopped, 1);
}
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    size_t n = strlen(g_test.log);
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_test.log + n, sizeof g_test.log - n, fmt, ap);
    va_end(ap);
}
/* gpu_driver_open with these settings: 1 when the run went on, 0 when it was stopped (g_test.fatal says why) */
static int run(const char* driver, const char* freedreno, void** gipa) {
    snprintf(g_settings.gpu_driver, sizeof g_settings.gpu_driver, "%s", driver);
    snprintf(g_settings.freedreno_variables, sizeof g_settings.freedreno_variables, "%s", freedreno);
    if (setjmp(g_test.stopped)) return 0;
    *gipa = gpu_driver_open();
    return 1;
}

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "";
    void* gipa = (void*)1;
    if (!strcmp(which, "env")) {
        /* the system's driver: nothing loaded, the variables set and said, the check a no-op */
        assert(run("", "TU_DEBUG=sysmem,nobin;FD_RD_DUMP=enable", &gipa) && !gipa && !gpu_driver_custom());
        assert(!strcmp(getenv("TU_DEBUG"), "sysmem,nobin") && !strcmp(getenv("FD_RD_DUMP"), "enable"));
        assert(strstr(g_test.log, "freedreno: TU_DEBUG=sysmem,nobin\n") &&
               strstr(g_test.log, "freedreno: FD_RD_DUMP=enable\n"));
        gpu_driver_check();
        assert(!gpu_driver_open()); /* once a run: the first one's answer */
    } else if (!strcmp(which, "badenv")) {
        /* a variable Freedreno does not read stops the run with its name, and nothing is set */
        assert(!run("", "TU_DEBUG=sysmem;LD_PRELOAD=/x.so", &gipa));
        assert(!strcmp(g_test.fatal, "freedreno_env=TU_DEBUG=sysmem;LD_PRELOAD=/x.so: \"LD_PRELOAD\" is not one of "
                                     "Freedreno's variables (capitals, digits and _, starting TU_, FD_ or IR3_)"));
        assert(!getenv("LD_PRELOAD") && !getenv("TU_DEBUG"));
    } else if (!strcmp(which, "here")) {
        /* a package asked for where none loads: stopped, saying this device and what it lacks */
        assert(!run("Turnip_v32", "", &gipa));
        assert(!strncmp(g_test.fatal, "GPU driver Turnip_v32 cannot load here. This device: ", 52));
        assert(strstr(g_test.fatal, "custom drivers load on Android 9 (API 28) or newer"));
    } else if (!strcmp(which, "badid")) {
        assert(!run("../escape", "", &gipa));
        assert(!strcmp(g_test.fatal,
                       "gpu_driver=../escape is no driver's folder name: letters, digits, '.', '-' and '_', "
                       "96 at most"));
    } else {
        fprintf(stderr, "usage: %s env|badenv|here|badid\n", argv[0]);
        return 2;
    }
    printf("gpu_driver_load %s: ok\n", which);
    return 0;
}
