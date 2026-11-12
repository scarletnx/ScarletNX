/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::nsd (src/nx/nn/nsd.c) on a stand-in CPU: each '%' of a name becomes the retail environment, lp1,
   every name resolves, the 0x100 bytes of the output are the name and zeros after it, a name with no end in its
   0x100 bytes is cut there, and the environment's own name fills its 8 bytes */
#include "nx/nn/nsd.c"
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

int main(void) {
    static CPU cpu;
    static u8 mem[0x400];
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
    printf("nn_nsd: %u checks passed\n", checks);
    return 0;
}
