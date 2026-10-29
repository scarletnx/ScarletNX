/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* link_macho.c's objects, compiled for arm64 macOS once per PART: PART 1 calls into PART 2 (branches, GOT loads,
   pointers in data to rebase, a weak import nothing defines, zerofill, constants, a 128-bit load and a signed byte
   one), as LLVM's modules do. The others are objects the linker refuses */
#if PART == 1
extern int counter;
int helper(int x);
extern int missing(void) __attribute__((weak_import));
const char* names[] = {"zero", "one", "two"};
int (*fns[])(int) = {helper, helper};
double scale[] = {1.5, 2.25};
unsigned long long zeroes[4096];
typedef int Vector __attribute__((vector_size(16)));
Vector vectors[2] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
signed char bytes[64] = {-1};
Vector second_vector(void);
Vector second_vector(void) { return vectors[1]; }
int odd_byte(void);
int odd_byte(void) { return bytes[33]; }
unsigned long long test_entry(void);
unsigned long long test_entry(void) {
    zeroes[4095] = 3;
    return (unsigned long long)(names[2][1] + fns[1](2) + counter + (missing ? 1 : 2)) + zeroes[4095] +
           (unsigned long long)scale[1];
}
#elif PART == 2
int counter = 10;
int helper(int x);
int helper(int x) { return x * 3; }
#elif PART == 3 /* refused: a call to a weak import nothing defines */
extern int missing(void) __attribute__((weak_import));
int test_entry(void);
int test_entry(void) { return missing ? missing() : 0; }
#elif PART == 4 /* refused: data aligned to more than a 16 KiB page */
_Alignas(32768) char big[16] = {1};
char* test_entry(void);
char* test_entry(void) { return big; }
#endif
