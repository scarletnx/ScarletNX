/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::nsd (src/nx/nn/nsd.c) on a stand-in CPU: each '%' of a name becomes the retail environment, lp1,
   every name resolves, the 0x100 bytes of the output are the name and zeros after it, a name with no end in its
   0x100 bytes is cut there, and the environment's own name fills its 8 bytes */
#include "nx/nn/nsd.c"
#include "hle/hook/hooks.hook.inc" /* nsd.hook's table, and a 32-bit game's */
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static CPU cpu;
static u8 mem[0x400];
CPU* hook_a32_cpu(SnxArmCPU* arm) { return (void)arm, &cpu; }

int main(void) {
    CPU* c = &cpu;
    cpu.mem = mem;
    strcpy((char*)mem, "accounts.%.ndas.srv.nintendo.net");
    memset(mem + 0x100, 0xee, 0x100);
    CHECK(nnnsd_resolve(c, 0x100, 0) == 0 && !strcmp((char*)mem + 0x100, "accounts.lp1.ndas.srv.nintendo.net"));
    CHECK(mem[0x1ff] == 0 && mem[0x100 + 34] == 0 && mem[0x100 + 35] == 0);
    strcpy((char*)mem, "example.invalid");
    CHECK(nnnsd_resolve_ex(c, 0x100, 0) == 0 && !strcmp((char*)mem + 0x100, "example.invalid"));
    memset(mem, 'a', 0x100); /* no end */
    memset(mem + 0x100, 0xee, 0x100);
    CHECK(nnnsd_resolve(c, 0x100, 0) == 0 && strlen((char*)mem + 0x100) == 0xff && mem[0x1ff] == 0);
    memset(mem, '%', 0x100); /* longer than 0x100 once replaced */
    CHECK(nnnsd_resolve(c, 0x100, 0) == 0 && strlen((char*)mem + 0x100) == 0xff && mem[0x1ff] == 0);
    CHECK(nnnsd_server_environment_type(c) == 1);
    memset(mem + 0x100, 0xee, 0x10); /* the environment's name in its 8 bytes, nothing past them */
    nnnsd_get_environment_identifier(c, 0x100);
    CHECK(!memcmp(mem + 0x100, "lp1\0\0\0\0\0", 8) && mem[0x108] == 0xee);
    /* a 32-bit game: each hook is in its table (nsd.hook's a32), the two names in r0 and r1 */
    {
        static SnxArmCPU a;
        CHECK(HOOK32_COUNT == HOOK_COUNT);
        /* each for the one 32-bit SDK read, 0.12.16, alone: a 32-bit game of another SDK has no hook here */
        for (int i = 0; i < HOOK32_COUNT; i++)
            CHECK(hook32_key[i] == HOOK_SDK && hook32_lo[i] == 0 && hook32_hi[i] == 0x10000);
        for (int i = 0; i < HOOK32_COUNT; i++) {
            CHECK(!strcmp(hook32_symbol[i], hook_symbol[i]));
            if (strcmp(hook32_symbol[i], "_ZN2nn3nsd7ResolveEPNS0_4FqdnERKS1_")) continue;
            strcpy((char*)mem, "a.%.example");
            a.r[0] = 0x100, a.r[1] = 0, a.r[14] = 0x2000;
            hook32_fn[i](&a);
            CHECK(a.r[0] == 0 && a.r[15] == 0x2000 && !strcmp((char*)mem + 0x100, "a.lp1.example"));
        }
    }
    printf("nn_nsd: %u checks passed\n", checks);
    return 0;
}
