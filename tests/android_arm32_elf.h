/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An ELF32 ARM library for the 32-bit bridge's tests (android_arm32, android_jni), written at a path */
#ifndef SNX_TESTS_ANDROID_ARM32_ELF_H
#define SNX_TESTS_ANDROID_ARM32_ELF_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void arm32_elf_le16(unsigned char* p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static inline void arm32_elf_le32(unsigned char* p, uint32_t v) {
    arm32_elf_le16(p, (uint16_t)v);
    arm32_elf_le16(p + 2, (uint16_t)(v >> 16));
}
static inline uint32_t arm32_elf_gnu_hash(const char* name) {
    uint32_t h = 5381;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) h = h * 33 + *p;
    return h;
}

enum { ARM32_ELF_SYSV = 1, ARM32_ELF_GNU = 2 };
/* A library of two A32 functions, answer() (42) and twice(x) (x + x), with the hash tables of hashes (SYSV, GNU or
   both), as one PT_LOAD of 320 bytes: the ELF header, two program headers, the code at 128, the symbols at 144, their
   names at 192, the SysV table at 208, the GNU one at 232 and the dynamic section at 264 */
static inline void arm32_elf_write(const char* path, int hashes) {
    unsigned char f[320] = {0};
    memcpy(f, "\177ELF\1\1\1", 7);
    arm32_elf_le16(f + 16, 3);  /* ET_DYN */
    arm32_elf_le16(f + 18, 40); /* EM_ARM */
    arm32_elf_le32(f + 20, 1);
    arm32_elf_le32(f + 28, 52);         /* e_phoff */
    arm32_elf_le32(f + 36, 0x5000000u); /* EABI 5 */
    arm32_elf_le16(f + 40, 52);
    arm32_elf_le16(f + 42, 32);
    arm32_elf_le16(f + 44, 2);
    arm32_elf_le16(f + 46, 40);
    unsigned char* ph = f + 52; /* PT_LOAD of the whole file, read, write and execute */
    arm32_elf_le32(ph, 1);
    arm32_elf_le32(ph + 16, sizeof f);
    arm32_elf_le32(ph + 20, sizeof f);
    arm32_elf_le32(ph + 24, 7);
    arm32_elf_le32(ph + 28, 0x1000);
    ph += 32; /* PT_DYNAMIC */
    arm32_elf_le32(ph, 2);
    arm32_elf_le32(ph + 4, 264);
    arm32_elf_le32(ph + 8, 264);
    arm32_elf_le32(ph + 16, 56);
    arm32_elf_le32(ph + 20, 56);
    arm32_elf_le32(ph + 24, 6);
    arm32_elf_le32(f + 128, 0xe3a0002au); /* answer: mov r0, #42 */
    arm32_elf_le32(f + 132, 0xe12fff1eu); /* bx lr */
    arm32_elf_le32(f + 136, 0xe0800000u); /* twice: add r0, r0, r0 */
    arm32_elf_le32(f + 140, 0xe12fff1eu); /* bx lr */
    static const char names[] = "\0answer\0twice";
    memcpy(f + 192, names, sizeof names);
    const uint32_t name_at[3] = {0, 1, 8}, value[3] = {0, 128, 136};
    for (unsigned i = 1; i < 3; ++i) {
        unsigned char* sym = f + 144 + 16 * i;
        arm32_elf_le32(sym, name_at[i]);
        arm32_elf_le32(sym + 4, value[i]);
        arm32_elf_le32(sym + 8, 8);
        sym[12] = 0x12; /* STB_GLOBAL, STT_FUNC */
        arm32_elf_le16(sym + 14, 1);
    }
    /* SysV: one bucket, its chain 1 then 2 */
    arm32_elf_le32(f + 208, 1);
    arm32_elf_le32(f + 212, 3);
    arm32_elf_le32(f + 216, 1);
    arm32_elf_le32(f + 220 + 4 * 1, 2);
    /* GNU: one bucket from symbol 1, a bloom word of 32 bits with both names' two bits, their hashes, the last odd */
    uint32_t h1 = arm32_elf_gnu_hash("answer"), h2 = arm32_elf_gnu_hash("twice"), shift = 5;
    uint32_t bloom = 1u << (h1 % 32) | 1u << ((h1 >> shift) % 32) | 1u << (h2 % 32) | 1u << ((h2 >> shift) % 32);
    arm32_elf_le32(f + 232, 1);
    arm32_elf_le32(f + 236, 1);
    arm32_elf_le32(f + 240, 1);
    arm32_elf_le32(f + 244, shift);
    arm32_elf_le32(f + 248, bloom);
    arm32_elf_le32(f + 252, 1);
    arm32_elf_le32(f + 256, h1 & ~1u);
    arm32_elf_le32(f + 260, h2 | 1u);
    unsigned char* d = f + 264;
    const uint32_t tags[][2] = {{5, 192}, {6, 144}, {10, sizeof names}, {11, 16}};
    for (unsigned i = 0; i < 4; ++i, d += 8) {
        arm32_elf_le32(d, tags[i][0]);
        arm32_elf_le32(d + 4, tags[i][1]);
    }
    if (hashes & ARM32_ELF_SYSV) {
        arm32_elf_le32(d, 4); /* DT_HASH */
        arm32_elf_le32(d + 4, 208);
        d += 8;
    }
    if (hashes & ARM32_ELF_GNU) {
        arm32_elf_le32(d, 0x6ffffef5u); /* DT_GNU_HASH */
        arm32_elf_le32(d + 4, 232);
    }
    FILE* out = fopen(path, "wb");
    if (!out || fwrite(f, 1, sizeof f, out) != sizeof f || fclose(out)) {
        fprintf(stderr, "arm32_elf: cannot write %s\n", path);
        exit(1);
    }
}
#endif
