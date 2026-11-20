/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* common/utilities/patches.c: IPS, IPS32 and IPSwitch patches written over a module as Eden writes them, each refusal
   saying its value, which patch a module takes by its build ID, the played mods' patches in their order, and the mods
   stamp they change */
#include "common/utilities/mods.h"
#include "common/utilities/patches.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 0x80 /* the module's image: the patches address it from SNX_PATCH_HEADER on */

static void put(const char* dir, const char* name, const void* data, size_t size) {
    char* path = snx_path_join(dir, name);
    assert(path && !snx_write_file(path, data, size));
    free(path);
}
static void put_text(const char* dir, const char* name, const char* text) { put(dir, name, text, strlen(text)); }
static int failed_with(int rc, const char* error, const char* want) {
    if (rc && !strcmp(error, want)) return 1;
    fprintf(stderr, "rc %d, error \"%s\", wanted \"%s\"\n", rc, error, want);
    return 0;
}
static int pchtxt(const char* text, unsigned char* image, char* error) {
    return snx_pchtxt_apply(text, strlen(text), image, SIZE, error, 256);
}

static void ips(void) {
    static const unsigned char patch[] = {'P',  'A',  'T',  'C',  'H',                          /* header */
                                          0x00, 0x01, 0x10, 0x00, 0x03, 0x11, 0x22, 0x33,       /* 3 bytes at 0x10 */
                                          0x00, 0x01, 0x20, 0x00, 0x00, 0x00, 0x04, 0xAA,       /* 4 of 0xAA at 0x20 */
                                          0x00, 0x00, 0xFE, 0x00, 0x04, 0x01, 0x02, 0x03, 0x04, /* 2 in the header */
                                          0x00, 0x00, 0x10, 0x00, 0x02, 0xEE, 0xEE, /* all of it in the header */
                                          'E',  'O',  'F'};
    static const unsigned char wide[] = {'I',  'P',  'S',  '3',  '2', 0x00, 0x00, 0x01,
                                         0x7F, 0x00, 0x01, 0x5A, 'E', 'E',  'O',  'F'};
    static const unsigned char past[] = {'P',  'A', 'T', 'C', 'H', 0x00, 0x01, 0x7E, 0x00,
                                         0x04, 1,   2,   3,   4,   'E',  'O',  'F'};
    static const unsigned char open[] = {'P', 'A', 'T', 'C', 'H', 0x00, 0x01, 0x10, 0x00, 0x02, 0x11};
    unsigned char image[SIZE] = {0};
    char error[256];
    size_t i;
    assert(!snx_ips_apply(patch, sizeof patch, image, SIZE, error, sizeof error));
    assert(image[0x10] == 0x11 && image[0x11] == 0x22 && image[0x12] == 0x33 && image[0x13] == 0);
    assert(image[0x20] == 0xAA && image[0x23] == 0xAA && image[0x24] == 0);
    assert(image[0] == 0x03 && image[1] == 0x04); /* what fell in the header is the header's */
    for (i = 2; i < SIZE; ++i) assert(image[i] != 0xEE);
    assert(!snx_ips_apply(wide, sizeof wide, image, SIZE, error, sizeof error) && image[0x7F] == 0x5A);
    memset(image, 0, sizeof image);
    assert(failed_with(snx_ips_apply(past, sizeof past, image, SIZE, error, sizeof error), error,
                       "record 1 writes 0x4 bytes at 0x17E, past the module's end at 0x180"));
    assert(!image[0x7E] && !image[0x7F]); /* nothing of it written */
    assert(failed_with(snx_ips_apply(open, sizeof open, image, SIZE, error, sizeof error), error,
                       "the IPS patch ends at its byte 11, inside its record 1"));
    assert(failed_with(snx_ips_apply(patch, 9, image, SIZE, error, sizeof error), error,
                       "the IPS patch ends at its byte 9, inside its record 1"));
    assert(failed_with(snx_ips_apply((const unsigned char*)"PATCH", 5, image, SIZE, error, sizeof error), error,
                       "the IPS patch ends at its byte 5, before the EOF that closes it"));
    assert(failed_with(snx_ips_apply((const unsigned char*)"NOTAPATCH", 9, image, SIZE, error, sizeof error), error,
                       "no IPS patch: it starts with neither PATCH nor IPS32"));
    puts("PASS IPS and IPS32: records, runs, the header's bytes, the module's end and a patch cut short");
}

static void ipswitch(void) {
    unsigned char image[SIZE] = {0}, id[32];
    char error[256], line[600];
    int i;
    const char* text = "@nsobid-ABCDEF0123\r\n"
                       "# a comment, and the flags\r\n"
                       "@flag print_values\r\n"
                       "@flag offset_shift 0x100\r\n"
                       "\r\n"
                       "@ENABLED\r\n"
                       "  00000010 1F2003D5   // spaces and a comment after it\r\n"
                       "00000020 \"hi\\n#\"\r\n"
                       "00000030 AABBEE\r\n"
                       "00000030 CCDD\r\n" /* the same offset again: this one, in the place of the other */
                       "@disabled\r\n"
                       "00000040 FFFF\r\n"
                       "@stop\r\n"
                       "anything at all\r\n";
    assert(snx_pchtxt_build_id(text, strlen(text), id));
    assert(id[0] == 0xAB && id[4] == 0x23 && !id[5] && !id[31]); /* the hex given, zeros after it */
    assert(!pchtxt(text, image, error));
    assert(image[0x10] == 0x1F && image[0x11] == 0x20 && image[0x12] == 0x03 && image[0x13] == 0xD5);
    assert(!memcmp(image + 0x20, "hi\n#", 4) && !image[0x24]); /* the escape and the # inside the quotes */
    assert(image[0x30] == 0xCC && image[0x31] == 0xDD && !image[0x32]);
    assert(!image[0x40]); /* @disabled */
    /* a line longer than 256 bytes (Eden's #4445) */
    memset(image, 0, sizeof image);
    i = snprintf(line, sizeof line, "@enabled\n@flag offset_shift 256\n00000000 ");
    while (i < 9 + 32 + 2 * SIZE) i += snprintf(line + i, sizeof line - i, "5A");
    assert(strlen(line) > 256 && !pchtxt(line, image, error) && image[0] == 0x5A && image[SIZE - 1] == 0x5A);
    /* no @nsobid, or one past @stop: none */
    assert(!snx_pchtxt_build_id("@enabled\n", 9, id) && !snx_pchtxt_build_id("@stop\n@nsobid-AB\n", 17, id));
    /* each refusal says the line and its value */
    assert(failed_with(pchtxt("@enabled\n@frobnicate\n", image, error), error,
                       "line 2: @frobnicate: the directives are @nsobid-, @enabled, @disabled, @flag, @little, @big "
                       "and @stop"));
    assert(failed_with(pchtxt("00000010 AABB\n", image, error), error,
                       "line 1: 00000010 AABB: a value outside any patch, with no @enabled or @disabled above it"));
    assert(failed_with(pchtxt("@enabled\n00000110 ABC\n", image, error), error,
                       "line 2: 00000110 ABC: the value ABC is not an even number of hex digits"));
    assert(failed_with(pchtxt("@enabled\n00000110 \"open\n", image, error), error,
                       "line 2: 00000110 \"open: a string with no closing \""));
    assert(failed_with(pchtxt("@enabled\n00000110\n", image, error), error,
                       "line 2: 00000110: an offset with no value after it"));
    assert(failed_with(pchtxt("@enabled\n0000017F AABB\n", image, error), error,
                       "line 2: 0x2 bytes at 0x17F end past the module's end at 0x180"));
    assert(failed_with(pchtxt("@flag offset_shift many\n", image, error), error,
                       "line 1: @flag offset_shift many: offset_shift takes a number"));
    assert(failed_with(pchtxt("@flag quiet\n", image, error), error,
                       "line 1: @flag quiet: the flags are offset_shift and print_values"));
    assert(failed_with(pchtxt("@nsobid-XYZ\n", image, error), error,
                       "line 1: @nsobid-XYZ: the build ID is up to 64 hex digits after @nsobid-"));
    assert(failed_with(pchtxt("@flag offset_shift -0x20\n@enabled\n00000010 AA\n", image, error), error,
                       "line 3: 00000010 AA: offset 0x10 with offset_shift -32 is below 0"));
    /* an offset and a shift whose sum no 63 bits hold: said as that, not added */
    assert(failed_with(pchtxt("@flag offset_shift 0x7fffffffffffffff\n@enabled\n00000010 AA\n", image, error), error,
                       "line 3: 00000010 AA: offset 0x10 with offset_shift 9223372036854775807 is past 2^63"));
    assert(failed_with(pchtxt("@enabled\nFFFFFFFFFFFFFFFF AA\n", image, error), error,
                       "line 2: FFFFFFFFFFFFFFFF AA: offset 0xFFFFFFFFFFFFFFFF with offset_shift 0 is past 2^63"));
    puts("PASS IPSwitch: its build ID, flags, comments, strings, disabled patches, a long line, @stop, refusals");
}

static void choice(const char* root) {
    unsigned char id[32] = {0xAB, 0xCD, 0xEF, 0x01, 0x23}, other[32] = {0x11};
    char error[256], *path, *dir = snx_path_join(root, "choice");
    char full[70];
    int i;
    assert(!snx_mkdirs(dir));
    put_text(dir, "a.pchtxt", "@nsobid-abcdef0123\n@enabled\n");
    put_text(dir, "b.pchtxt", "@nsobid-1100\n@enabled\n");
    for (i = 0; i < 32; ++i) snprintf(full + 2 * i, 3, "%02X", id[i]);
    snprintf(full + 64, sizeof full - 64, ".ips");
    {
        const char* named[] = {"ABCDEF0123.ips", "abcdef0123.IPS", "ABCDEF012300.ips", full};
        for (i = 0; i < 4; ++i) {
            path = snx_path_join(dir, named[i]);
            assert(snx_patch_for(path, id, error, sizeof error) == 1 && !snx_patch_for(path, other, error, 256));
            free(path);
        }
    }
    path = snx_path_join(dir, "ABCDEF0124.ips");
    assert(!snx_patch_for(path, id, error, sizeof error));
    free(path);
    path = snx_path_join(dir, "a.pchtxt");
    assert(snx_patch_for(path, id, error, sizeof error) == 1 && !snx_patch_for(path, other, error, sizeof error));
    free(path);
    path = snx_path_join(dir, "b.pchtxt");
    assert(!snx_patch_for(path, id, error, sizeof error) && snx_patch_for(path, other, error, sizeof error) == 1);
    free(path);
    path = snx_path_join(dir, "main");
    assert(!snx_patch_for(path, id, error, sizeof error)); /* no patch */
    free(path);
    path = snx_path_join(dir, "gone.pchtxt");
    assert(snx_patch_for(path, id, error, sizeof error) == -1 && strstr(error, "cannot read ") &&
           strstr(error, "gone.pchtxt: "));
    free(path);
    /* a file's patch: its error starts with the file */
    {
        unsigned char image[SIZE] = {0};
        char want[600];
        put_text(dir, "bad.pchtxt", "@enabled\n00000110 ABC\n");
        path = snx_path_join(dir, "bad.pchtxt");
        snprintf(want, sizeof want, "%s: line 2: 00000110 ABC: the value ABC is not an even number of hex digits",
                 path);
        assert(failed_with(snx_patch_apply(path, image, SIZE, error, sizeof error), error, want));
        free(path);
    }
    /* a module's patches: those for its build written in their order, each marked, another build's not */
    {
        static const unsigned char first[] = {'P', 'A', 'T', 'C', 'H', 0, 1, 0, 0, 2, 0x11, 0x55, 'E', 'O', 'F'};
        static const unsigned char other_build[] = {'P', 'A', 'T', 'C', 'H', 0, 1, 1, 0, 1, 0x22, 'E', 'O', 'F'};
        unsigned char image[SIZE] = {0}, applied[3];
        char *list[3], want[600];
        put(dir, "ABCDEF0123.ips", first, sizeof first);
        put(dir, "1100.ips", other_build, sizeof other_build);
        put_text(dir, "c.pchtxt", "@nsobid-abcdef0123\n@flag offset_shift 0x100\n@enabled\n00000002 33\n00000000 44\n");
        list[0] = snx_path_join(dir, "ABCDEF0123.ips");
        list[1] = snx_path_join(dir, "1100.ips");
        list[2] = snx_path_join(dir, "c.pchtxt");
        assert(!snx_patch_module(image, SIZE, id, list, 3, applied, error, sizeof error));
        assert(applied[0] && !applied[1] && applied[2]);
        assert(image[0] == 0x44 && image[1] == 0x55 && image[2] == 0x33); /* the .pchtxt after the .ips, over it */
        free(list[2]);
        put_text(dir, "worse.pchtxt", "@nsobid-abcdef0123\n@enabled\n00000110 ABC\n");
        list[2] = snx_path_join(dir, "worse.pchtxt");
        snprintf(want, sizeof want, "%s: line 3: 00000110 ABC: the value ABC is not an even number of hex digits",
                 list[2]);
        assert(failed_with(snx_patch_module(image, SIZE, id, list, 3, NULL, error, sizeof error), error, want));
        for (i = 0; i < 3; ++i) free(list[i]);
    }
    free(dir);
    puts("PASS which patch a module takes: a .ips by its name in any case, a .pchtxt by its @nsobid");
}

static void played(const char* root) {
    char *mods = snx_path_join(root, "mods"), *dir, **paths, *before, *after, *again, *off;
    const char* tails[3];
    size_t n, i;
    static const char* const layout[][2] = {{"b_mod", "z.pchtxt"},   {"b_mod", "A.ips"}, {"b_mod", "main"},
                                            {"b_mod", "notes.txt"},  {"a_mod", "y.ips"}, {"off_mod", "q.ips"},
                                            {"b_mod", ".hidden.ips"}};
    for (i = 0; i < sizeof layout / sizeof *layout; ++i) {
        char* mod = snx_path_join(mods, layout[i][0]);
        dir = snx_path_join(mod, "exefs");
        assert(!snx_mkdirs(dir));
        put_text(dir, layout[i][1], "@enabled\n");
        free(dir);
        free(mod);
    }
    /* the played mods in their names' order, each one's .ips and .pchtxt by name; a module, a note, a hidden file and
       a mod turned off out */
    n = snx_mods_patches(mods, "off_mod", &paths);
    tails[0] = "a_mod/exefs/y.ips";
    tails[1] = "b_mod/exefs/A.ips";
    tails[2] = "b_mod/exefs/z.pchtxt";
    assert(n == 3);
    for (i = 0; i < n; ++i) { /* in the system's separators, which the check reads as / */
        size_t k;
        for (k = 0; paths[i][k]; ++k)
            if (paths[i][k] == '\\') paths[i][k] = '/';
        assert(strlen(paths[i]) >= strlen(tails[i]) &&
               !strcmp(paths[i] + strlen(paths[i]) - strlen(tails[i]), tails[i]));
    }
    snx_free_strings(paths, n);
    /* the stamp: a patch changes it, its bytes too, and a mod turned off gives back the one without it */
    off = snx_format("off_mod%cb_mod", SNX_MODS_SEPARATOR);
    before = snx_mods_stamp(mods, off); /* a_mod's y.ips alone */
    after = snx_mods_stamp(mods, "off_mod");
    assert(before[0] && after[0] && strcmp(before, after));
    dir = snx_path_join(mods, "b_mod");
    {
        char* exefs = snx_path_join(dir, "exefs");
        put_text(exefs, "z.pchtxt", "@enabled\n00000110 AA\n");
        free(exefs);
    }
    again = snx_mods_stamp(mods, "off_mod");
    assert(strcmp(again, after));
    free(again);
    again = snx_mods_stamp(mods, off);
    assert(!strcmp(again, before));
    free(again);
    free(before);
    free(after);
    free(off);
    free(dir);
    free(mods);
    puts("PASS the played mods' patches in their order, and the mods stamp they change");
}

int main(void) {
    char* root = snx_temp_dir(NULL);
    assert(root);
    ips();
    ipswitch();
    choice(root);
    played(root);
    snx_remove_tree(root);
    free(root);
    return 0;
}
