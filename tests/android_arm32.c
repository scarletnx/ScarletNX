/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit native bridge (src/android/bridge/arm32*.c): its loader on ELF32 ARM libraries the test writes, and
   bionic's functions as 32-bit guest code calls them, through each import's export and a call into the guest
   (arm32_call), against what bionic gives on Android 16 */
#include "android/bridge/arm32.h"
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
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
static uint32_t word_at(uint32_t address) {
    uint32_t w;
    memcpy(&w, G(address), sizeof w);
    return w;
}
/* the guest string at an address, "" for NULL */
static const char* string_at(uint32_t address) { return address ? G(address) : ""; }
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

/* soft-float: a double in two words, a float in one */
static uint32_t f_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}
static uint64_t d_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}
#define D(d) (uint32_t)d_bits(d), (uint32_t)(d_bits(d) >> 32)
/* name called with words, its double result from r0 and r1 */
static uint64_t call64(const char* name, const uint32_t* words, unsigned count) {
    uint32_t fn = arm32_hle_symbol(name), high = 0;
    if (!fn) {
        fprintf(stderr, "android_arm32: the 32-bit bridge has no %s\n", name);
        g_t.failures++;
        return 0;
    }
    uint32_t low = arm32_call(fn, words, count, &high);
    return low | (uint64_t)high << 32;
}
#define CALL64(name, ...) call64(name, WORDS(__VA_ARGS__), COUNT(__VA_ARGS__))

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

/* ---- files ---- */

/* a host path in the test's folder, made a guest string */
static uint32_t in_dir(const char* name) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", g_t.dir, name);
    return text(path);
}
static void make_file(const char* name, const char* contents) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", g_t.dir, name);
    FILE* f = fopen(path, "wb");
    if (!f || fputs(contents, f) < 0 || fclose(f)) {
        fprintf(stderr, "android_arm32: cannot write %s\n", path);
        exit(1);
    }
}

static void test_files(void) {
    /* open's flags are ARM's, which AArch64's are: O_DIRECTORY 040000, O_NOFOLLOW 0100000 */
    make_file("plain", "plain");
    char target[160], link_path[160];
    snprintf(target, sizeof target, "%s/plain", g_t.dir);
    snprintf(link_path, sizeof link_path, "%s/plain-link", g_t.dir);
    if (symlink("plain", link_path)) {
        fprintf(stderr, "android_arm32: cannot make %s\n", link_path);
        exit(1);
    }
    expect_int("open of a file with O_DIRECTORY", (int32_t)CALL("open", in_dir("plain"), 040000, 0), -1);
    expect_int("open of a file with O_DIRECTORY: errno ENOTDIR", guest_errno(), 20);
    expect_int("open of a link with O_NOFOLLOW", (int32_t)CALL("open", in_dir("plain-link"), 0100000, 0), -1);
    expect_int("open of a link with O_NOFOLLOW: errno ELOOP", guest_errno(), 40);
    int32_t fd = (int32_t)CALL("open", in_dir("plain-link"), 0, 0);
    expect_int("open of a link follows it", fd >= 0, 1);
    if (fd >= 0) close(fd);
    remove(link_path);
    remove(target);
}

/* the file name's contents, read back by the host */
static void expect_contents(const char* what, const char* name, const char* want) {
    char path[160], got[64] = {0};
    snprintf(path, sizeof path, "%s/%s", g_t.dir, name);
    FILE* f = fopen(path, "rb");
    size_t n = f ? fread(got, 1, sizeof got - 1, f) : 0;
    if (f) fclose(f);
    got[n] = 0;
    expect_text(what, got, want);
}
/* what a descriptor reads from its start, closed after: the file's contents, "" for none */
static void expect_reads(const char* what, int32_t fd, const char* want) {
    char got[64] = {0};
    ssize_t n = fd > 2 ? pread(fd, got, sizeof got - 1, 0) : -1;
    if (fd > 2) close(fd);
    if (n < 0) n = 0;
    got[n] = 0;
    expect_text(what, got, want);
}
static bool exists(const char* name) {
    char path[160];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", g_t.dir, name);
    return !lstat(path, &st);
}

/* the calls whose 64-bit handler the shim once named and the 64-bit bridge never had */
static void test_calls(void) {
    make_file("data", "abcdef");
    int32_t fd = (int32_t)CALL("open64", in_dir("data"), 2 /* O_RDWR */, 0);
    expect_int("open64 opens a descriptor of its own", fd > 2, 1);
    expect_int("pwrite", CALL("pwrite", (uint32_t)fd, text("XY"), 2, 2), 2);
    expect_contents("pwrite writes at its offset", "data", "abXYef");
    uint32_t buf = zeroed(16);
    expect_int("pread64", CALL("pread64", (uint32_t)fd, buf, 3, 0, 3, 0), 3);
    expect_text("pread64 reads at its offset", G(buf), "Yef");
    expect_int("pwrite64", CALL("pwrite64", (uint32_t)fd, text("Z"), 1, 0, 0, 0), 1);
    expect_contents("pwrite64 writes at its offset", "data", "ZbXYef");
    expect_int("fdatasync", CALL("fdatasync", (uint32_t)fd), 0);
    expect_int("ftruncate64", CALL("ftruncate64", (uint32_t)fd, 0, 3, 0), 0);
    expect_contents("ftruncate64 cuts", "data", "ZbX");
    close(fd);

    uint32_t missing = in_dir("absent");
    expect_int("openat of a missing file", (int32_t)CALL("openat", (uint32_t)-100, missing, 0, 0), -1);
    expect_int("openat of a missing file: errno ENOENT", guest_errno(), 2);
    expect_reads("__openat_2 opens the file", (int32_t)CALL("__openat_2", (uint32_t)-100, in_dir("data"), 0), "ZbX");
    int dir = open(g_t.dir, O_RDONLY | O_DIRECTORY);
    expect_reads("openat in a folder's descriptor", (int32_t)CALL("openat", (uint32_t)dir, text("data"), 0, 0), "ZbX");
    fd = (int32_t)CALL("creat", in_dir("made"), 0600);
    expect_int("creat makes the file", fd >= 0 && exists("made"), 1);
    if (fd >= 0) close(fd);
    expect_int("faccessat of a file", CALL("faccessat", (uint32_t)-100, in_dir("made"), 0, 0), 0);
    expect_int("faccessat of a missing file", (int32_t)CALL("faccessat", (uint32_t)-100, missing, 0, 0), -1);
    expect_int("faccessat of a missing file: errno ENOENT", guest_errno(), 2);
    expect_int("faccessat with a flag Linux lacks", (int32_t)CALL("faccessat", (uint32_t)-100, in_dir("made"), 0, 8),
               -1);
    expect_int("faccessat with a flag Linux lacks: errno EINVAL", guest_errno(), 22);
    expect_int("chown to the same owner", CALL("chown", in_dir("made"), (uint32_t)-1, (uint32_t)-1), 0);
    expect_int("symlink", CALL("symlink", text("made"), in_dir("made-link")), 0);
    expect_int("symlink makes a link", exists("made-link"), 1);
    expect_int("link", CALL("link", in_dir("made"), in_dir("made-hard")), 0);
    expect_int("link makes a name", exists("made-hard"), 1);
    expect_int("unlinkat of a file", CALL("unlinkat", (uint32_t)dir, text("made-link"), 0), 0);
    expect_int("unlinkat removes it", exists("made-link"), 0);
    mkdirat(dir, "sub", 0700);
    expect_int("unlinkat of a folder with AT_REMOVEDIR", CALL("unlinkat", (uint32_t)dir, text("sub"), 0x200), 0);
    expect_int("unlinkat removes the folder", exists("sub"), 0);
    uint32_t pattern = in_dir("tmpXXXXXX");
    fd = (int32_t)CALL("mkstemp", pattern);
    expect_int("mkstemp makes a file", fd >= 0 && strstr(G(pattern), "XXXXXX") == NULL, 1);
    if (fd >= 0) {
        close(fd);
        remove(G(pattern));
    }

    int here = open(".", O_RDONLY | O_DIRECTORY);
    expect_int("fchdir", CALL("fchdir", (uint32_t)dir), 0);
    expect_int("fchdir moves into the folder", access("data", F_OK), 0);
    if (fchdir(here)) exit(1);
    close(here);
    close(dir);

    uint32_t fds = zeroed(8);
    expect_int("pipe2", CALL("pipe2", fds, 02000000 | 04000), 0);
    int ends[2];
    memcpy(ends, G(fds), sizeof ends);
    expect_int("pipe2's ends are close-on-exec", (fcntl(ends[0], F_GETFD) & FD_CLOEXEC) != 0, 1);
    expect_int("pipe2's ends do not block", (fcntl(ends[1], F_GETFL) & O_NONBLOCK) != 0, 1);
    close(ends[0]);
    close(ends[1]);
    expect_int("pipe2 with a flag it lacks", (int32_t)CALL("pipe2", fds, 1), -1);
    expect_int("pipe2 with a flag it lacks: errno EINVAL", guest_errno(), 22);

    mode_t before = umask(022);
    expect_int("umask gives the mask before", CALL("umask", 077), 022);
    expect_int("umask sets it", umask(before), 077);
    expect_int("getpgrp", CALL("getpgrp"), getpgrp());
    errno = 0;
    expect_int("getpriority", (int32_t)CALL("getpriority", 0, 0), getpriority(PRIO_PROCESS, 0));
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) _exit(CALL("setsid") == (uint32_t)getpid() ? 0 : 1);
    int status = 0;
    waitpid(pid, &status, 0);
    expect_int("setsid in a child makes its session", WIFEXITED(status) && WEXITSTATUS(status) == 0, 1);
#if defined(__linux__) || defined(__FreeBSD__)
    fd = (int32_t)CALL("eventfd", 5, 0);
    uint64_t count = 0;
    expect_int("eventfd's count", fd >= 0 && read(fd, &count, sizeof count) == 8 ? (int64_t)count : -1, 5);
    if (fd >= 0) close(fd);
#else
    expect_int("eventfd on a host without it", (int32_t)CALL("eventfd", 5, 0), -1);
    expect_int("eventfd on a host without it: errno ENOSYS", guest_errno(), 38);
#endif
    remove(G(in_dir("made")));
    remove(G(in_dir("made-hard")));
    remove(G(in_dir("data")));
}

/* ---- getline, getdelim, remove, mkdtemp and getpagesize ---- */

static void test_stdio(void) {
    FILE* host = tmpfile();
    if (!host) exit(1);
    fputs("first\nsecond", host);
    rewind(host);
    uint32_t file = arm32_guest_file(host), line = zeroed(4), size = zeroed(4);
    expect_int("getline of the first line", CALL("getline", line, size, file), 6);
    expect_text("getline keeps the newline", string_at(word_at(line)), "first\n");
    expect_int("getline grows the buffer", word_at(size) >= 7, 1);
    expect_int("getline of the last line", CALL("getline", line, size, file), 6);
    expect_text("getline of a line with no newline", string_at(word_at(line)), "second");
    expect_int("getline at the end", (int32_t)CALL("getline", line, size, file), -1);
    expect_int("getline with no stream", (int32_t)CALL("getline", line, size, 0), -1);
    expect_int("getline with no stream: errno EINVAL", guest_errno(), 22);
    rewind(host);
    expect_int("getdelim to an 'r'", CALL("getdelim", line, size, 'r', file), 3);
    expect_text("getdelim stops at its delimiter", string_at(word_at(line)), "fir");
    fclose(host);
    make_file("gone", "x");
    expect_int("remove", CALL("remove", in_dir("gone")), 0);
    expect_int("remove removes", exists("gone"), 0);
    expect_int("remove of a missing file", (int32_t)CALL("remove", in_dir("gone")), -1);
    expect_int("remove of a missing file: errno ENOENT", guest_errno(), 2);
    uint32_t pattern = in_dir("dXXXXXX");
    expect_int("mkdtemp gives its template", CALL("mkdtemp", pattern), pattern);
    struct stat st;
    expect_int("mkdtemp makes a folder", !stat(G(pattern), &st) && S_ISDIR(st.st_mode), 1);
    rmdir(G(pattern));
    uint32_t page = CALL("getpagesize");
    expect_int("getpagesize is sysconf(_SC_PAGESIZE)", page, CALL("sysconf", 39));
    expect_int("getpagesize is a power of two from 4096", page >= 4096 && (page & (page - 1)) == 0, 1);
}

/* ---- numbers from text: bionic's ranges and errno ---- */

static void clear_errno(void) { memset(G(arm32_errno_address()), 0, 4); }
/* name(s, end, base) for a long: its value, and the characters it read into *used */
static uint32_t to_long(const char* name, const char* s, int base, uint32_t* used) {
    uint32_t text_at = text(s), end = zeroed(4);
    clear_errno();
    uint32_t v = CALL(name, text_at, end, (uint32_t)base);
    *used = word_at(end) ? word_at(end) - text_at : 0;
    return v;
}

static void test_numbers(void) {
    uint32_t used;
    expect_int("strtol of 99999999999", (int32_t)to_long("strtol", "99999999999", 10, &used), INT32_MAX);
    expect_int("strtol of 99999999999: errno ERANGE", guest_errno(), 34);
    expect_int("strtol of -99999999999", (int32_t)to_long("strtol", "-99999999999", 10, &used), INT32_MIN);
    expect_int("strtol of -99999999999: errno ERANGE", guest_errno(), 34);
    expect_int("strtol of 0x1F in base 0", to_long("strtol", "0x1F", 0, &used), 31);
    expect_int("strtol of 0x1F in base 0 sets no errno", guest_errno(), 0);
    expect_int("strtoul of -1", to_long("strtoul", "-1", 10, &used), UINT32_MAX);
    expect_int("strtoul of -1 sets no errno", guest_errno(), 0);
    expect_int("strtoul of -2", to_long("strtoul", "-2", 10, &used), 0xfffffffe);
    expect_int("strtoul of 4294967296", to_long("strtoul", "4294967296", 10, &used), UINT32_MAX);
    expect_int("strtoul of 4294967296: errno ERANGE", guest_errno(), 34);
    expect_int("strtoul of -4294967296", to_long("strtoul", "-4294967296", 10, &used), UINT32_MAX);
    expect_int("strtoul of -4294967296: errno ERANGE", guest_errno(), 34);
    expect_int("strtoul of \" 017\" in base 0", to_long("strtoul", " 017", 0, &used), 15);
    expect_int("strtoul of \" 017\" reads 4 characters", used, 4);
    expect_int("strtoul of \"-x\" reads nothing", to_long("strtoul", "-x", 10, &used) == 0 && used == 0, 1);
    uint32_t end = zeroed(4), big = text("1e400"), hex = text("0x1.8p3");
    clear_errno();
    uint64_t inf = CALL64("strtod", big, end);
    expect_int("strtod of 1e400", (int64_t)inf, (int64_t)d_bits(HUGE_VAL));
    expect_int("strtod of 1e400: errno ERANGE", guest_errno(), 34);
    clear_errno();
    expect_int("strtod of 0x1.8p3", (int64_t)CALL64("strtod", hex, end), (int64_t)d_bits(12.0));
    expect_int("strtod of 0x1.8p3 reads it all", word_at(end), hex + 7);
    expect_int("strtod of 0x1.8p3 sets no errno", guest_errno(), 0);
    clear_errno();
    expect_int("atof of 1e400", (int64_t)CALL64("atof", big), (int64_t)d_bits(HUGE_VAL));
    expect_int("atof of 1e400: errno ERANGE", guest_errno(), 34);
    clear_errno();
    expect_int("strtof of 1e50", CALL("strtof", text("1e50"), end), f_bits(HUGE_VALF));
    expect_int("strtof of 1e50: errno ERANGE", guest_errno(), 34);
    clear_errno();
    expect_int("strtoll of 99999999999999999999", (int64_t)CALL64("strtoll", text("99999999999999999999"), end, 10),
               INT64_MAX);
    expect_int("strtoll of 99999999999999999999: errno ERANGE", guest_errno(), 34);
}

/* ---- multibyte characters: UTF-8, as bionic's C.UTF-8 ---- */

static void test_multibyte(void) {
    uint32_t utf8 = text("a\xc3\xa7\xf0\x9f\x98\x80"), wide = zeroed(32), back = zeroed(16);
    expect_int("mbstowcs counts", CALL("mbstowcs", 0, utf8, 0), 3);
    expect_int("mbstowcs", CALL("mbstowcs", wide, utf8, 8), 3);
    expect_int("mbstowcs: a", word_at(wide), 0x61);
    expect_int("mbstowcs: c cedilla", word_at(wide + 4), 0xe7);
    expect_int("mbstowcs: U+1F600", word_at(wide + 8), 0x1f600);
    expect_int("mbstowcs ends with a NUL", word_at(wide + 12), 0);
    expect_int("mbstowcs of a stray byte", (int32_t)CALL("mbstowcs", wide, text("a\xff"), 8), -1);
    expect_int("mbstowcs of a stray byte: errno EILSEQ", guest_errno(), 84);
    expect_int("wcstombs counts", CALL("wcstombs", 0, wide, 0), 7);
    expect_int("wcstombs", CALL("wcstombs", back, wide, 16), 7);
    expect_text("wcstombs gives the UTF-8 back", G(back), G(utf8));
    expect_int("MB_CUR_MAX", CALL("__ctype_get_mb_cur_max"), 4);
}

/* ---- libm ---- */

static void test_libm(void) {
    expect_int("expm1(1e-10)", (int64_t)CALL64("expm1", D(1e-10)), (int64_t)d_bits(expm1(1e-10)));
    expect_int("expm1f(1e-5)", CALL("expm1f", f_bits(1e-5f)), f_bits(expm1f(1e-5f)));
    expect_int("log1p(1e-10)", (int64_t)CALL64("log1p", D(1e-10)), (int64_t)d_bits(log1p(1e-10)));
    expect_int("log1pf(1e-5)", CALL("log1pf", f_bits(1e-5f)), f_bits(log1pf(1e-5f)));
    expect_int("fmax(-0.5, 2)", (int64_t)CALL64("fmax", D(-0.5), D(2.0)), (int64_t)d_bits(2.0));
    expect_int("fmaxf(NAN, 1)", CALL("fmaxf", f_bits(NAN), f_bits(1.0f)), f_bits(1.0f));
    expect_int("fmin(-0.5, 2)", (int64_t)CALL64("fmin", D(-0.5), D(2.0)), (int64_t)d_bits(-0.5));
    expect_int("fminf(3, -1)", CALL("fminf", f_bits(3.0f), f_bits(-1.0f)), f_bits(-1.0f));
    expect_int("nextafter(1, 2)", (int64_t)CALL64("nextafter", D(1.0), D(2.0)), 0x3ff0000000000001ll);
    expect_int("nextafterf(1, 2)", CALL("nextafterf", f_bits(1.0f), f_bits(2.0f)), 0x3f800001);
    /* fma keeps the product's rounding error: 0x1.000002p0 * 0x1.fffffcp-1 - 1 */
    float a = 0x1.000002p0f, b = 0x1.fffffcp-1f;
    expect_int("fmaf(a, b, -1)", CALL("fmaf", f_bits(a), f_bits(b), f_bits(-1.0f)), f_bits(fmaf(a, b, -1.0f)));
    expect_int("fmaf is fused", CALL("fmaf", f_bits(a), f_bits(b), f_bits(-1.0f)) != f_bits(a * b - 1.0f), 1);
    expect_int("fma(0.1, 10, -1)", (int64_t)CALL64("fma", D(0.1), D(10.0), D(-1.0)), (int64_t)d_bits(fma(0.1, 10, -1)));
    expect_int("lround(2.5)", (int32_t)CALL("lround", D(2.5)), 3);
    expect_int("lround(-2.5)", (int32_t)CALL("lround", D(-2.5)), -3);
    expect_int("lroundf(-1.5)", (int32_t)CALL("lroundf", f_bits(-1.5f)), -2);
    expect_int("llround(1e12 + 0.5)", (int64_t)CALL64("llround", D(1e12 + 0.5)), 1000000000001ll);
    expect_int("llroundf(-2.5)", (int64_t)CALL64("llroundf", f_bits(-2.5f)), -3);
}

/* ---- signals: bionic's 32-bit sigset_t, a bit for each of the signals 1 to 32 ---- */

static void test_signals(void) {
    uint32_t set = zeroed(4);
    memset(G(set), 0x5a, 4);
    expect_int("sigemptyset", CALL("sigemptyset", set), 0);
    expect_int("sigemptyset empties", word_at(set), 0);
    expect_int("sigaddset SIGUSR1", CALL("sigaddset", set, 10), 0);
    expect_int("sigaddset SIGHUP", CALL("sigaddset", set, 1), 0);
    expect_int("sigaddset sets the bits", word_at(set), 1u << 9 | 1u);
    expect_int("sigismember SIGUSR1", CALL("sigismember", set, 10), 1);
    expect_int("sigismember SIGUSR2", CALL("sigismember", set, 12), 0);
    expect_int("sigdelset SIGHUP", CALL("sigdelset", set, 1), 0);
    expect_int("sigdelset clears its bit", word_at(set), 1u << 9);
    expect_int("sigaddset of signal 33", (int32_t)CALL("sigaddset", set, 33), -1);
    expect_int("sigaddset of signal 33: errno EINVAL", guest_errno(), 22);
    expect_int("sigismember of signal 0", (int32_t)CALL("sigismember", set, 0), -1);
    expect_int("sigfillset", CALL("sigfillset", set), 0);
    expect_int("sigfillset fills", word_at(set), UINT32_MAX);
    /* SIGUSR1 blocked and raised: pending until it is let through. Ignored, so its run does nothing */
    uint32_t usr1 = zeroed(4), pending = zeroed(4);
    CALL("sigaddset", usr1, 10);
    CALL("signal", 10, 1 /* SIG_IGN */);
    expect_int("sigprocmask blocks SIGUSR1", CALL("sigprocmask", 0 /* SIG_BLOCK */, usr1, 0), 0);
    expect_int("raise SIGUSR1", CALL("raise", 10), 0);
    expect_int("sigpending", CALL("sigpending", pending), 0);
    expect_int("sigpending holds the blocked SIGUSR1", word_at(pending), 1u << 9);
    CALL("sigprocmask", 1 /* SIG_UNBLOCK */, usr1, 0);
}

/* ---- the shim: every name it sends to the 64-bit bridge has a handler there ---- */

static void test_shim(void) {
    const char* names[16];
    unsigned count = arm32_shim_unresolved(names, 16);
    for (unsigned i = 0; i < count && i < 16; ++i)
        fprintf(stderr, "android_arm32: the shim sends %s to the 64-bit bridge, which has no handler for it\n",
                names[i]);
    expect_int("the shim's names without a 64-bit handler", count, 0);
    /* GLES 3's unsigned uniforms (a GLES 3 shader's uint uniforms), the 64-bit bridge's */
    static const char* const gles3[] = {"glUniform1ui",  "glUniform2ui",  "glUniform3ui",  "glUniform4ui",
                                        "glUniform1uiv", "glUniform2uiv", "glUniform3uiv", "glUniform4uiv"};
    for (size_t i = 0; i < sizeof gles3 / sizeof *gles3; ++i) {
        char what[64];
        snprintf(what, sizeof what, "%s is an import", gles3[i]);
        expect_int(what, arm32_hle_symbol(gles3[i]) != 0, 1);
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

/* ---- the C locale: the host's numbers and names never reach the guest ---- */

/* with the host in a locale of a decimal comma when it has one, as the JVM sets the user's */
static void test_locale(void) {
    const char* comma = NULL;
    static const char* const commas[] = {"pt_BR.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8", "es_ES.UTF-8",
                                         "it_IT.UTF-8", "ru_RU.UTF-8", "nl_NL.UTF-8"};
    for (size_t i = 0; i < sizeof commas / sizeof *commas && !comma; ++i)
        if (setlocale(LC_ALL, commas[i]) && !strcmp(localeconv()->decimal_point, ",")) comma = commas[i];
    if (!comma) {
        setlocale(LC_ALL, "C");
        printf("android_arm32: the host has no locale with a decimal comma: the guest's numbers and names are checked "
               "in the host's C locale alone\n");
    }
    uint32_t out = zeroed(64);
    CALL("snprintf", out, 64, text("%.2f|%e"), 0, D(3.14159), D(12345.678));
    expect_text("snprintf of doubles", G(out), "3.14|1.234568e+04");
    uint32_t d = zeroed(8);
    expect_int("sscanf of a double", CALL("sscanf", text("3.5 x"), text("%lf"), d), 1);
    double got;
    memcpy(&got, G(d), sizeof got);
    expect_int("sscanf's double times 10", (int64_t)(got * 10), 35);
    expect_int("strtod of 1.25", (int64_t)CALL64("strtod", text("1.25"), 0), (int64_t)d_bits(1.25));
    /* bionic's 32-bit struct tm: sec, min, hour, mday, mon, year, wday, yday, isdst, gmtoff, zone */
    uint32_t tm = zeroed(44);
    const uint32_t fields[9] = {0, 0, 0, 14, 10, 123, 2, 317, 0}; /* Tuesday 2023-11-14 */
    memcpy(G(tm), fields, sizeof fields);
    CALL("strftime", out, 64, text("%a %b %A %B"), tm);
    expect_text("strftime's names", G(out), "Tue Nov Tuesday November");
    expect_text("the host's decimal point after", localeconv()->decimal_point, comma ? "," : ".");
    setlocale(LC_ALL, "C");
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
    test_shim();
    test_fortify();
    test_files();
    test_calls();
    test_stdio();
    test_multibyte();
    test_numbers();
    test_locale();
    test_libm();
    test_signals();
    rmdir(g_t.dir);
    if (g_t.failures) fprintf(stderr, "android_arm32: %d check(s) failed\n", g_t.failures);
    return g_t.failures ? 1 : 0;
}
