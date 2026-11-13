/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/nds_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Assembled for ARMv5TE at 0x02000000:

   entry:  ldr r0, =0x03003FFC      @ the DTCM's last word, where the BIOS finds the handler
           adr r1, irq_handler
           str r1, [r0]
           mov r0, #0x04000000
           mov r1, #8               @ DISPSTAT: the vertical blank's interrupt
           strh r1, [r0, #4]
           add r2, r0, #0x200
           mov r1, #1
           str r1, [r2, #0x10]      @ IE
           str r1, [r2, #8]         @ IME
           mrs r3, cpsr
           bic r3, r3, #0x80
           msr cpsr_c, r3
           ldr r4, =0x02100000
           mov r5, #0x100
           clz r6, r5
           str r6, [r4]             @ 23
           mvn r5, #0x80000000
           qadd r6, r5, r5
           str r6, [r4, #4]         @ 0x7FFFFFFF
           ldr r0, =thumb_fn+1
           blx r0
           str r0, [r4, #8]         @ 0x1234
           mov r0, #100
           mov r1, #7
           swi 0x090000             @ Div
           str r0, [r4, #12]        @ 14
           str r1, [r4, #16]        @ 2
           blx thumb_two
           str r0, [r4, #28]        @ 42
   loop:   swi 0x050000             @ VBlankIntrWait
           ldr r0, [r4, #20]
           add r0, r0, #1
           str r0, [r4, #20]
           b loop
   irq_handler:
           mov r0, #0x04000000
           add r0, r0, #0x200
           ldr r1, [r0, #0x14]
           str r1, [r0, #0x14]      @ IF acknowledged
           ldr r2, =0x03003FF8
           ldr r3, [r2]
           orr r3, r3, r1
           str r3, [r2]             @ the BIOS's flags
           ldr r2, =0x02100018
           ldr r3, [r2]
           add r3, r3, #1
           str r3, [r2]
           bx lr
           .ltorg
           .thumb
   thumb_fn:
           ldr r0, =0x1234
           bx lr
   thumb_two:
           push {r4, lr}
           movs r4, #40
           bl thumb_leaf
           adds r0, r0, r4
           pop {r4, pc}
   thumb_leaf:
           movs r0, #2
           bx lr */
const uint32_t nds_test_program[] = {
    0xE59F00B8, 0xE28F1080, 0xE5801000, 0xE3A00301, 0xE3A01008, 0xE1C010B4, 0xE2802C02, 0xE3A01001, 0xE5821010,
    0xE5821008, 0xE10F3000, 0xE3C33080, 0xE121F003, 0xE3A04621, 0xE3A05C01, 0xE16F6F15, 0xE5846000, 0xE3E05102,
    0xE1056055, 0xE5846004, 0xE59F006C, 0xE12FFF30, 0xE5840008, 0xE3A00064, 0xE3A01007, 0xEF090000, 0xE584000C,
    0xE5841010, 0xFA000017, 0xE584001C, 0xEF050000, 0xE5940014, 0xE2800001, 0xE5840014, 0xEAFFFFFA, 0xE3A00301,
    0xE2800C02, 0xE5901014, 0xE5801014, 0xE59F2024, 0xE5923000, 0xE1833001, 0xE5823000, 0xE59F2018, 0xE5923000,
    0xE2833001, 0xE5823000, 0xE12FFF1E, 0x03003FFC, 0x020000D1, 0x03003FF8, 0x02100018, 0x47704804, 0x2428B510,
    0xF802F000, 0xBD101900, 0x47702002, 0x00001234};
const size_t nds_test_program_words = sizeof nds_test_program / sizeof *nds_test_program;

static void put32(unsigned char* at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) at[i] = (unsigned char)(value >> (i * 8));
}

bool nds_test_image(const char* path, const uint32_t* words, size_t count) {
    enum { PROGRAM = 0x8000, SIZE = 0x10000 };
    unsigned char* image = calloc(1, SIZE);
    if (!image || count * 4 > SIZE - PROGRAM) {
        free(image);
        return false;
    }
    memcpy(image, "SNXTEST", 7);
    memcpy(image + 12, "TEST", 4);
    put32(image + 0x20, PROGRAM);          /* the ARM9 program's place in the image */
    put32(image + 0x24, NDS_TEST_ADDRESS); /* its entry */
    put32(image + 0x28, NDS_TEST_ADDRESS); /* where it is loaded */
    put32(image + 0x2C, count * 4 < 0x800 ? 0x800u : (uint32_t)(count * 4));
    for (size_t i = 0; i < count; ++i) put32(image + PROGRAM + i * 4, words[i]);
    FILE* f = fopen(path, "wb");
    bool ok = f && fwrite(image, 1, SIZE, f) == SIZE;
    if (f) ok = !fclose(f) && ok;
    free(image);
    return ok;
}
