/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit native bridge (src/android/bridge/arm32*.c): its loader on ELF32 ARM libraries the test writes, and
   bionic's functions as 32-bit guest code calls them, through each import's export and a call into the guest
   (arm32_call), against what bionic gives on Android 16 */
#include "android/bridge/arm32.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static struct {
    int failures;
    char dir[64]; /* the libraries the test writes */
} g_t;

static void expect_int(const char* what, int64_t got, int64_t want) {
    if (got != want) {
        fprintf(stderr, "android_arm32: %s: got %lld (0x%llx), want %lld (0x%llx)\n", what, (long long)got,
                (unsigned long long)got, (long long)want, (unsigned long long)want);
        g_t.failures++;
    }
}

static void expect_text(const char* what, const char* got, const char* want) {
    if (strcmp(got, want)) {
        fprintf(stderr, "android_arm32: %s: got \"%s\", want \"%s\"\n", what, got, want);
        g_t.failures++;
    }
}

/* ---- calls as guest code makes them ---- */

/* the import name called with its argument words (r0 to r3, then the stack): r0. 0 and a failure when the bridge lacks
   it */
static uint32_t call(const char* name, const uint32_t* words, unsigned count) {
    uint32_t fn = arm32_hle_symbol(name);
    if (!fn) {
        fprintf(stderr, "android_arm32: the 32-bit bridge has no %s\n", name);
        g_t.failures++;
        return 0;
    }
    return arm32_call(fn, words, count, NULL);
}
#define WORDS(...)      ((const uint32_t[]){__VA_ARGS__})
#define COUNT(...)      (sizeof WORDS(__VA_ARGS__) / sizeof(uint32_t))
#define CALL(name, ...) call(name, WORDS(__VA_ARGS__), COUNT(__VA_ARGS__))
#define G(address)      ((char*)arm32_host(address))

/* a guest copy of a string, and a zeroed guest buffer */
static uint32_t text(const char* s) { return arm32_strdup(s); }
static uint32_t zeroed(uint32_t size) {
    uint32_t a = arm32_malloc(size);
    memset(G(a), 0, size);
    return a;
}
static uint32_t guest_errno(void) {
    uint32_t e;
    memcpy(&e, G(arm32_errno_address()), sizeof e);
    return e;
}

/* name called with words in a child process, which must end on SIGABRT with want in what it printed */
static void expect_abort(const char* name, const uint32_t* words, unsigned count, const char* want) {
    int out[2];
    fflush(NULL);
    if (pipe(out)) {
        fprintf(stderr, "android_arm32: no pipe for %s's child\n", name);
        exit(1);
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(out[1], 2);
        close(out[0]);
        call(name, words, count);
        _exit(0);
    }
    close(out[1]);
    char said[512];
    size_t got = 0;
    ssize_t n;
    while (got < sizeof said - 1 && (n = read(out[0], said + got, sizeof said - 1 - got)) > 0) got += (size_t)n;
    said[got] = 0;
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT || !strstr(said, want)) {
        fprintf(stderr, "android_arm32: %s: wanted an abort that says \"%s\", got %s %d and \"%s\"\n", name, want,
                WIFSIGNALED(status) ? "signal" : "exit status",
                WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status), said);
        g_t.failures++;
    }
}
#define ABORTS(name, want, ...) expect_abort(name, WORDS(__VA_ARGS__), COUNT(__VA_ARGS__), want)

/* ---- the loader ---- */

static void le16(unsigned char* p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void le32(unsigned char* p, uint32_t v) {
    le16(p, (uint16_t)v);
    le16(p + 2, (uint16_t)(v >> 16));
}
static uint32_t gnu_hash_of(const char* name) {
    uint32_t h = 5381;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) h = h * 33 + *p;
    return h;
}

enum { SYSV = 1, GNU = 2 };
/* A library of two A32 functions, answer() (42) and twice(x) (x + x), with the hash tables of hashes (SYSV, GNU or
   both), as one PT_LOAD of 320 bytes: the ELF header, two program headers, the code at 128, the symbols at 144, their
   names at 192, the SysV table at 208, the GNU one at 232 and the dynamic section at 264 */
static void write_library(const char* path, int hashes) {
    unsigned char f[320] = {0};
    memcpy(f, "\177ELF\1\1\1", 7);
    le16(f + 16, 3);  /* ET_DYN */
    le16(f + 18, 40); /* EM_ARM */
    le32(f + 20, 1);
    le32(f + 28, 52);         /* e_phoff */
    le32(f + 36, 0x5000000u); /* EABI 5 */
    le16(f + 40, 52);
    le16(f + 42, 32);
    le16(f + 44, 2);
    le16(f + 46, 40);
    unsigned char* ph = f + 52; /* PT_LOAD of the whole file, read, write and execute */
    le32(ph, 1);
    le32(ph + 16, sizeof f);
    le32(ph + 20, sizeof f);
    le32(ph + 24, 7);
    le32(ph + 28, 0x1000);
    ph += 32; /* PT_DYNAMIC */
    le32(ph, 2);
    le32(ph + 4, 264);
    le32(ph + 8, 264);
    le32(ph + 16, 56);
    le32(ph + 20, 56);
    le32(ph + 24, 6);
    le32(f + 128, 0xe3a0002au); /* answer: mov r0, #42 */
    le32(f + 132, 0xe12fff1eu); /* bx lr */
    le32(f + 136, 0xe0800000u); /* twice: add r0, r0, r0 */
    le32(f + 140, 0xe12fff1eu); /* bx lr */
    static const char names[] = "\0answer\0twice";
    memcpy(f + 192, names, sizeof names);
    const uint32_t name_at[3] = {0, 1, 8}, value[3] = {0, 128, 136};
    for (unsigned i = 1; i < 3; ++i) {
        unsigned char* sym = f + 144 + 16 * i;
        le32(sym, name_at[i]);
        le32(sym + 4, value[i]);
        le32(sym + 8, 8);
        sym[12] = 0x12; /* STB_GLOBAL, STT_FUNC */
        le16(sym + 14, 1);
    }
    /* SysV: one bucket, its chain 1 then 2 */
    le32(f + 208, 1);
    le32(f + 212, 3);
    le32(f + 216, 1);
    le32(f + 220 + 4 * 1, 2);
    /* GNU: one bucket from symbol 1, a bloom word of 32 bits with both names' two bits, their hashes, the last odd */
    uint32_t h1 = gnu_hash_of("answer"), h2 = gnu_hash_of("twice"), shift = 5;
    uint32_t bloom = 1u << (h1 % 32) | 1u << ((h1 >> shift) % 32) | 1u << (h2 % 32) | 1u << ((h2 >> shift) % 32);
    le32(f + 232, 1);
    le32(f + 236, 1);
    le32(f + 240, 1);
    le32(f + 244, shift);
    le32(f + 248, bloom);
    le32(f + 252, 1);
    le32(f + 256, h1 & ~1u);
    le32(f + 260, h2 | 1u);
    unsigned char* d = f + 264;
    const uint32_t tags[][2] = {{5, 192}, {6, 144}, {10, sizeof names}, {11, 16}};
    for (unsigned i = 0; i < 4; ++i, d += 8) {
        le32(d, tags[i][0]);
        le32(d + 4, tags[i][1]);
    }
    if (hashes & SYSV) {
        le32(d, 4); /* DT_HASH */
        le32(d + 4, 208);
        d += 8;
    }
    if (hashes & GNU) {
        le32(d, 0x6ffffef5u); /* DT_GNU_HASH */
        le32(d + 4, 232);
    }
    FILE* out = fopen(path, "wb");
    if (!out || fwrite(f, 1, sizeof f, out) != sizeof f || fclose(out)) {
        fprintf(stderr, "android_arm32: cannot write %s\n", path);
        exit(1);
    }
}

/* each library's functions found by name and called, whatever hash tables it has */
static void test_loader(void) {
    static const struct {
        const char* name;
        int hashes;
    } libraries[] = {{"libsysv.so", SYSV}, {"libgnu.so", GNU}, {"libboth.so", SYSV | GNU}};
    for (size_t i = 0; i < sizeof libraries / sizeof *libraries; ++i) {
        char path[128], what[160], error[256];
        snprintf(path, sizeof path, "%s/%s", g_t.dir, libraries[i].name);
        write_library(path, libraries[i].hashes);
        const Arm32Library* l = arm32_load(path, error, sizeof error);
        if (!l) {
            fprintf(stderr, "android_arm32: %s does not load: %s\n", libraries[i].name, error);
            g_t.failures++;
            continue;
        }
        uint32_t answer = arm32_symbol(l, "answer"), twice = arm32_symbol(l, "twice");
        snprintf(what, sizeof what, "%s: answer is found", libraries[i].name);
        expect_int(what, answer != 0, 1);
        snprintf(what, sizeof what, "%s: twice is found", libraries[i].name);
        expect_int(what, twice != 0, 1);
        snprintf(what, sizeof what, "%s: a name it lacks is not", libraries[i].name);
        expect_int(what, arm32_symbol(l, "absent"), 0);
        if (answer) {
            snprintf(what, sizeof what, "%s: answer()", libraries[i].name);
            expect_int(what, arm32_call(answer, NULL, 0, NULL), 42);
        }
        if (twice) {
            const uint32_t x = 21;
            snprintf(what, sizeof what, "%s: twice(21)", libraries[i].name);
            expect_int(what, arm32_call(twice, &x, 1, NULL), 42);
        }
        remove(path);
    }
}

/* ---- the FORTIFY forms: the function within the buffer, and bionic's abort past it ---- */

static void test_fortify(void) {
    uint32_t dst = zeroed(32), src = text("abcdefghijklmnop"), hello = text("hello");
    expect_int("__memcpy_chk returns dst", CALL("__memcpy_chk", dst, src, 8, 16), dst);
    expect_text("__memcpy_chk copies", G(dst), "abcdefgh");
    ABORTS("__memcpy_chk", "memcpy: prevented 20-byte write into 10-byte buffer", dst, src, 20, 10);
    CALL("__memmove_chk", dst + 1, dst, 4, 16);
    expect_text("__memmove_chk moves", G(dst), "aabcdfgh");
    ABORTS("__memmove_chk", "memmove: prevented 20-byte write into 10-byte buffer", dst, src, 20, 10);
    CALL("__memset_chk", dst, 'x', 3, 16);
    expect_text("__memset_chk sets", G(dst), "xxxcdfgh");
    ABORTS("__memset_chk", "memset: prevented 17-byte write into 16-byte buffer", dst, 0, 17, 16);
    expect_int("__memchr_chk finds", CALL("__memchr_chk", hello, 'l', 5, 8), hello + 2);
    ABORTS("__memchr_chk", "memchr: prevented 9-byte read from 8-byte buffer", hello, 'z', 9, 8);
    expect_int("__strlen_chk", CALL("__strlen_chk", hello, 6), 5);
    ABORTS("__strlen_chk", "strlen: detected read past end of 5-byte buffer", hello, 5);

    memset(G(dst), 0, 32);
    CALL("__strcpy_chk", dst, hello, 16);
    expect_text("__strcpy_chk copies", G(dst), "hello");
    ABORTS("__strcpy_chk", "strcpy: prevented 6-byte write into 5-byte buffer", dst, hello, 5);
    uint32_t ab = zeroed(8);
    strcpy(G(ab), "ab");
    CALL("__strcat_chk", ab, text("cd"), 8);
    expect_text("__strcat_chk appends", G(ab), "abcd");
    ABORTS("__strcat_chk", "strcat: prevented write past end of 8-byte buffer", ab, text("efgh"), 8);
    memset(G(dst), 'z', 8);
    CALL("__strncpy_chk", dst, text("hi"), 4, 8);
    expect_int("__strncpy_chk pads with NULs", memcmp(G(dst), "hi\0\0zzzz", 8), 0);
    ABORTS("__strncpy_chk", "strncpy: prevented 9-byte write into 8-byte buffer", dst, hello, 9, 8);
    memset(G(dst), 'z', 8);
    CALL("__strncpy_chk2", dst, text("hi"), 4, 8, 3);
    expect_int("__strncpy_chk2 pads with NULs", memcmp(G(dst), "hi\0\0zzzz", 8), 0);
    ABORTS("__strncpy_chk2", "strncpy: detected read past end of 3-byte buffer", dst, src, 6, 8, 3);
    expect_int("__strchr_chk finds", CALL("__strchr_chk", hello, 'l', 6), hello + 2);
    expect_int("__strchr_chk stops at the NUL", CALL("__strchr_chk", hello, 'z', 6), 0);
    ABORTS("__strchr_chk", "strchr: prevented read past end of 3-byte buffer", hello, 'z', 3);
    expect_int("__strrchr_chk finds the last", CALL("__strrchr_chk", hello, 'l', 6), hello + 3);
    ABORTS("__strrchr_chk", "strrchr: prevented read past end of 3-byte buffer", hello, 'z', 3);

    /* stdio, on a guest FILE of a host temporary file */
    FILE* host = tmpfile();
    if (!host) {
        fprintf(stderr, "android_arm32: no temporary file\n");
        exit(1);
    }
    fputs("line\nabcdef", host);
    rewind(host);
    uint32_t file = arm32_guest_file(host);
    memset(G(dst), 0, 32);
    expect_int("__fgets_chk returns dst", CALL("__fgets_chk", dst, 16, file, 16), dst);
    expect_text("__fgets_chk reads a line", G(dst), "line\n");
    ABORTS("__fgets_chk", "fgets: prevented 16-byte write into 8-byte buffer", dst, 16, file, 8);
    ABORTS("__fgets_chk", "fgets: buffer size -1 < 0", dst, (uint32_t)-1, file, 8);
    expect_int("__fread_chk", CALL("__fread_chk", dst, 1, 4, file, 8), 4);
    expect_int("__fread_chk reads", memcmp(G(dst), "abcd", 4), 0);
    ABORTS("__fread_chk", "fread: prevented 12-byte write into 8-byte buffer", dst, 4, 3, file, 8);
    expect_int("__fread_chk of a size that overflows", CALL("__fread_chk", dst, 0x10000, 0x10000, file, 8), 0);
    expect_int("__fread_chk of a size that overflows: errno EOVERFLOW", guest_errno(), 75);
    expect_int("__fwrite_chk", CALL("__fwrite_chk", hello, 1, 3, file, 8), 3);
    ABORTS("__fwrite_chk", "fwrite: prevented 9-byte read from 8-byte buffer", hello, 3, 3, file, 8);

    /* __vsnprintf_chk's va_list is the address of its words */
    uint32_t va = zeroed(8);
    G(va)[0] = 42;
    memset(G(dst), 0, 32);
    expect_int("__vsnprintf_chk", CALL("__vsnprintf_chk", dst, 16, 0, 16, text("n=%d"), va), 4);
    expect_text("__vsnprintf_chk formats", G(dst), "n=42");
    ABORTS("__vsnprintf_chk", "vsnprintf: prevented 16-byte write into 8-byte buffer", dst, 16, 0, 8, text("n=%d"), va);

    /* __open_2 and __openat_2: open without a mode, refused for O_CREAT (ARM's 0100) */
    uint32_t missing = text("/nonexistent/android-arm32");
    expect_int("__open_2 of a missing file", (int32_t)CALL("__open_2", missing, 0), -1);
    expect_int("__open_2 of a missing file: errno ENOENT", guest_errno(), 2);
    ABORTS("__open_2", "open: called with O_CREAT/O_TMPFILE but no mode (flags 0101)", missing, 0101);
    ABORTS("__openat_2", "open: called with O_CREAT/O_TMPFILE but no mode (flags 020040002)", (uint32_t)-100, missing,
           020040002);

    /* readlink, sendto and recvfrom are the 64-bit bridge's after the check */
    char link_path[128];
    snprintf(link_path, sizeof link_path, "%s/link", g_t.dir);
    if (symlink("target-name", link_path)) {
        fprintf(stderr, "android_arm32: cannot make %s\n", link_path);
        exit(1);
    }
    uint32_t link = text(link_path);
    memset(G(dst), 0, 32);
    expect_int("__readlink_chk", CALL("__readlink_chk", link, dst, 32, 32), 11);
    expect_text("__readlink_chk reads the link", G(dst), "target-name");
    ABORTS("__readlink_chk", "readlink: prevented 32-byte write into 16-byte buffer", link, dst, 32, 16);
    remove(link_path);
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds)) {
        fprintf(stderr, "android_arm32: no socket pair\n");
        exit(1);
    }
    expect_int("__sendto_chk", CALL("__sendto_chk", (uint32_t)fds[0], text("ping"), 4, 8, 0, 0, 0), 4);
    memset(G(dst), 0, 32);
    expect_int("__recvfrom_chk", CALL("__recvfrom_chk", (uint32_t)fds[1], dst, 16, 16, 0, 0, 0), 4);
    expect_text("__recvfrom_chk receives", G(dst), "ping");
    ABORTS("__sendto_chk", "sendto: prevented 9-byte read from 8-byte buffer", (uint32_t)fds[0], src, 9, 8, 0, 0, 0);
    ABORTS("__recvfrom_chk", "recvfrom: prevented 16-byte write into 8-byte buffer", (uint32_t)fds[1], dst, 16, 8, 0, 0,
           0);
    close(fds[0]);
    close(fds[1]);
}

int main(void) {
    char error[256];
    if (!arm32_init(error, sizeof error)) {
        fprintf(stderr, "android_arm32: the 32-bit bridge does not start: %s\n", error);
        return 1;
    }
    snprintf(g_t.dir, sizeof g_t.dir, "android-arm32-XXXXXX");
    if (!mkdtemp(g_t.dir)) {
        fprintf(stderr, "android_arm32: no folder for the test's libraries in the working folder\n");
        return 1;
    }
    test_loader();
    test_fortify();
    rmdir(g_t.dir);
    if (g_t.failures) fprintf(stderr, "android_arm32: %d check(s) failed\n", g_t.failures);
    return g_t.failures ? 1 : 0;
}
