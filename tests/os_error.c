/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_os_error: the system's reason for the file or folder call that just failed, on Windows too, whose calls set
   GetLastError and not errno */
#include "common/utilities/common.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* s, UTF-8 the test trusts, as the UTF-16 units FormatMessageW gives, into out (64 at most); their count */
static size_t utf16(const char* s, uint16_t* out) {
    const unsigned char* p = (const unsigned char*)s;
    size_t n = 0;
    while (*p) {
        uint32_t c = *p < 0x80 ? *p : *p < 0xE0 ? *p & 0x1F : *p < 0xF0 ? *p & 0x0F : *p & 0x07;
        int more = *p < 0x80 ? 0 : *p < 0xE0 ? 1 : *p < 0xF0 ? 2 : 3;
        for (++p; more--; ++p) c = c << 6 | (*p & 0x3F);
        if (c >= 0x10000) {
            out[n++] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else
            out[n++] = (uint16_t)c;
        assert(n < 64);
    }
    return n;
}
/* Windows' text for code, given as UTF-8 and ending as FormatMessageW's do, is said as want */
static void says(const char* text, unsigned long code, size_t size, const char* want) {
    uint16_t units[64];
    char out[256];
    size_t n = utf16(text, units);
    assert(size <= sizeof out);
    memset(out, 'x', sizeof out);
    snx_windows_reason(units, n, code, out, size);
    if (strcmp(out, want)) {
        fprintf(stderr, "os_error.c: %s (%lu) in %zu bytes is \"%s\", not \"%s\"\n", text, code, size, out, want);
        abort();
    }
    for (n = size; n < sizeof out; ++n) assert(out[n] == 'x'); /* nothing past size */
}

int main(void) {
    char *root = snx_temp_dir(NULL), *file, *under, *missing, why[256];
    FILE* f;
    assert(root);
    file = snx_path_join(root, "a file");
    under = snx_path_join(file, "a folder");
    missing = snx_path_join(root, "missing");
    assert(file && under && missing && !snx_write_file(file, "x", 1));
    /* a folder under a file: the file is where its parent goes, which Windows' CreateDirectoryW says without errno */
    errno = 0;
    assert(snx_mkdirs(under));
    snx_os_error(why, sizeof why);
#ifdef _WIN32
    assert(why[0] && !strstr(why, "successfully"));
#else
    assert(!strcmp(why, strerror(EEXIST)));
#endif
    printf("a folder under a file: %s\n", why);
    /* a file that is not there */
    errno = 0;
    f = snx_fopen(missing, "rb");
    assert(!f);
    snx_os_error(why, sizeof why);
#ifdef _WIN32
    assert(why[0] && !strstr(why, "successfully"));
#else
    assert(!strcmp(why, strerror(ENOENT)));
#endif
    printf("a file not there: %s\n", why);
    /* a buffer too small is cut, never overrun */
    snx_os_error(why, 4);
    assert(strlen(why) <= 3);
    assert(!snx_remove_tree(root));
    free(missing);
    free(under);
    free(file);
    free(root);
    /* Windows' reason in the system's language, UTF-16 from FormatMessageW, comes out as UTF-8 with its code, never in
       the system's code page, which every reader of it takes for UTF-8; its line break and full stop left out */
    says("O sistema n\xc3\xa3o pode encontrar o caminho especificado.\r\n", 3, 256,
         "O sistema n\xc3\xa3o pode encontrar o caminho especificado (Windows error 3)");
    says("\xe6\x8c\x87\xe5\xae\x9a\xe3\x81\x95\xe3\x82\x8c\xe3\x81\x9f\xe3\x83\x91\xe3\x82\xb9\xe3\x81\x8c\xe8\xa6\x8b"
         "\xe3\x81\xa4\xe3\x81\x8b\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82\r\n",
         3, 256,
         "\xe6\x8c\x87\xe5\xae\x9a\xe3\x81\x95\xe3\x82\x8c\xe3\x81\x9f\xe3\x83\x91\xe3\x82\xb9\xe3\x81\x8c\xe8\xa6\x8b"
         "\xe3\x81\xa4\xe3\x81\x8b\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93 (Windows error 3)");
    says("\xd0\x9e\xd1\x82\xd0\xba\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xbd\xd0\xbe \xd0\xb2 "
         "\xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf\xd0\xb5.\r\n",
         5, 256,
         "\xd0\x9e\xd1\x82\xd0\xba\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xbd\xd0\xbe \xd0\xb2 "
         "\xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf\xd0\xb5 (Windows error 5)");
    says("U+1F600 \xf0\x9f\x98\x80\r\n", 87, 256, "U+1F600 \xf0\x9f\x98\x80 (Windows error 87)"); /* a surrogate pair */
    says("", 1392, 256, "Windows error 1392");                                                    /* no text */
    says("\r\n", 1392, 256, "Windows error 1392");
    { /* a unit that is no character, a surrogate alone: U+FFFD; a 0 ends the text */
        static const uint16_t lone[] = {'a', 0xD800, 'b', 0xDC00, 0, 'c'};
        char out[64];
        snx_windows_reason(lone, 6, 2, out, sizeof out);
        assert(!strcmp(out, "a\xef\xbf\xbd"
                            "b\xef\xbf\xbd (Windows error 2)"));
    }
    /* a short out: whole characters of the text, and the code after them only whole */
    says("O sistema n\xc3\xa3o pode encontrar o caminho especificado.\r\n", 3, 32,
         "O sistema n\xc3\xa3 (Windows error 3)");
    says("O sistema n\xc3\xa3o pode encontrar o caminho especificado.\r\n", 3, 31, "O sistema n (Windows error 3)");
    says("\xe6\x8c\x87\xe5\xae\x9a\xe3\x81\x95\xe3\x82\x8c\r\n", 3, 8, "\xe6\x8c\x87\xe5\xae\x9a");
    says("\xe6\x8c\x87\xe5\xae\x9a\xe3\x81\x95\xe3\x82\x8c\r\n", 3, 3, "");
    says("", 1392, 8, "Windows");
#ifdef _WIN32
    /* the text FormatMessageW gave, its code with it */
    SetLastError(ERROR_PATH_NOT_FOUND);
    snx_os_error(why, sizeof why);
    printf("Windows error 3 here: %s\n", why);
    assert(strstr(why, " (Windows error 3)") && strlen(why) > strlen(" (Windows error 3)"));
#endif
    puts("PASS snx_os_error says the system's reason");
    return 0;
}
