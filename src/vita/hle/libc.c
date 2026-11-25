/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceLibc, SceLibm and SceLibstdcxx, the C and C++ libraries (Dinkumware's, as SCE builds them) a retail game brings
   in its sce_module/, answered here as the system's libraries are, with SceLibKernel's sceClib* beside them: malloc on
   a heap in guest memory (heap.c) as large as the game's sceLibcParam asks, strings in place, printf and scanf reading
   the guest's arguments (AAPCS: words in r0 to r3 then on the stack, doublewords at even ones; a va_list the address
   of the next), stdio on the game's files (io.c), each thread's copy of the module's thread-local data. The
   variables they export (_Ctype, _Stdout, the exceptions' vtables) get their contents in their slots. The functions
   are the vitasdk database's names (MIT); the layouts are Dinkumware's as the game's code reads them */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include "vita/runtime/watch.h"
#include "common/utilities/common.h"
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE 0x1000u
#define DEFAULT_HEAP 0x4000000u /* when the game's sceLibcParam names no size */
#define ENOMEM 12u
#define EINVAL 22u
#define FILE_MAGIC 0x454c4946u /* "FILE": a FILE the runtime made, then its descriptor */
#define STDOUT_FD 0xffff0001u
#define STDERR_FD 0xffff0002u
/* the libc's data page (p->libc.data): the ctype tables (EOF first, then 256 entries), gmtime's struct tm, a vtable
   whose functions do nothing */
#define CTYPE_TABLE  0x000u
#define LOWER_TABLE  0x210u
#define UPPER_TABLE  0x420u
#define TM_RESULT    0x630u
#define EMPTY_VTABLE 0x660u
#define DATA_SIZE    0x1000u
/* Dinkumware's ctype bits */
#define C_XD 0x01
#define C_UP 0x02
#define C_SP 0x04
#define C_PU 0x08
#define C_LO 0x10
#define C_DI 0x20
#define C_CN 0x40
#define C_BB 0x80

static char* host(VitaProcess* p, uint32_t address) { return (char*)vita_ptr(p, address); }
static uint32_t guest(VitaProcess* p, const void* pointer) {
    return pointer ? (uint32_t)((const unsigned char*)pointer - p->mem) : 0;
}
static void set_errno(VitaProcess* p, VitaThread* t, uint32_t e) { vita_write32(p, t->tls + VITA_TLS_ERRNO, e); }
/* the game's fatal end: its libraries' abort */
static uint32_t fatal(VitaProcess* p, VitaThread* t, const char* what) {
    printf("vita: the game stopped: %s (thread \"%s\", lr 0x%08" PRIx32 ")\n", what, t->name, t->cpu.r[14]);
    p->exited = true;
    p->exit_code = -1;
    return 0;
}

/* ---- the libc's data and heap ---- */
static uint32_t data(VitaProcess* p) {
    if (p->libc.data) return p->libc.data;
    uint32_t d = vita_alloc(p, &p->user, DATA_SIZE, PAGE);
    if (!d) return 0;
    for (int c = 0; c < 256; ++c) {
        uint32_t bits = 0;
        if (isdigit(c)) bits |= C_DI | C_XD;
        if (isupper(c)) bits |= C_UP;
        if (islower(c)) bits |= C_LO;
        if (isxdigit(c) && !isdigit(c)) bits |= C_XD;
        if (ispunct(c)) bits |= C_PU;
        if (c == ' ') bits |= C_SP;
        if (c >= '\t' && c <= '\r') bits |= C_CN;
        else if (iscntrl(c)) bits |= C_BB;
        if (c >= 0x80) bits = 0; /* the "C" locale's */
        vita_write16(p, d + CTYPE_TABLE + 2 + (uint32_t)c * 2, bits);
        vita_write16(p, d + LOWER_TABLE + 2 + (uint32_t)c * 2, c < 0x80 ? (uint32_t)tolower(c) : (uint32_t)c);
        vita_write16(p, d + UPPER_TABLE + 2 + (uint32_t)c * 2, c < 0x80 ? (uint32_t)toupper(c) : (uint32_t)c);
    }
    vita_write16(p, d + LOWER_TABLE, 0xffff), vita_write16(p, d + UPPER_TABLE, 0xffff);
    for (uint32_t i = 2; i < 8; ++i) vita_write32(p, d + EMPTY_VTABLE + i * 4, VITA_RETURN);
    return p->libc.data = d;
}
/* sceProcessParam's sceLibcParam's heap size (its third word points at it), when the game names one */
static uint32_t heap_size(VitaProcess* p) {
    uint32_t param = p->image.process_param, libc = param && vita_mapped(p, param + 0x2c, 4) ? vita_read32(p, param + 0x2c) : 0;
    uint32_t pointer = libc && vita_mapped(p, libc, 12) ? vita_read32(p, libc + 8) : 0;
    uint32_t size = pointer && vita_mapped(p, pointer, 4) ? vita_read32(p, pointer) : 0;
    return size ? size : DEFAULT_HEAP;
}
static uint32_t heap(VitaProcess* p) {
    if (p->libc.heap) return p->libc.heap;
    uint32_t size = heap_size(p), base = vita_alloc(p, &p->user, size, PAGE);
    if (!base) {
        printf("vita: no room for SceLibc's heap of %" PRIu32 " MiB\n", size >> 20);
        return 0;
    }
    return p->libc.heap = vita_heap_init(p, base, size);
}
uint32_t vita_libc_lc_malloc(VitaProcess* p, VitaThread* t, uint32_t size) {
    uint32_t r = vita_heap_alloc(p, heap(p), size, 8);
    if (!r) set_errno(p, t, ENOMEM);
    return r;
}
uint32_t vita_libc_lc_calloc(VitaProcess* p, VitaThread* t, uint32_t nmemb, uint32_t member_size) {
    (void)t;
    uint64_t size = (uint64_t)nmemb * member_size;
    uint32_t r = size >> 31 ? 0 : vita_heap_alloc(p, heap(p), (uint32_t)size, 8);
    if (r) memset(host(p, r), 0, (size_t)size);
    return r;
}
uint32_t vita_libc_lc_free(VitaProcess* p, VitaThread* t, uint32_t ptr) {
    (void)t;
    vita_heap_free(p, heap(p), ptr);
    return 0;
}
uint32_t vita_libc_lc_realloc(VitaProcess* p, VitaThread* t, uint32_t ptr, uint32_t size) {
    uint32_t r = vita_heap_realloc(p, heap(p), ptr, size, 8);
    if (!r && size) set_errno(p, t, ENOMEM);
    return r;
}
uint32_t vita_libc_lc_memalign(VitaProcess* p, VitaThread* t, uint32_t align, uint32_t size) {
    (void)t;
    return vita_heap_alloc(p, heap(p), size, align);
}
/* reallocalign(ptr, size, alignment) */
uint32_t vita_libc_lc_reallocalign(VitaProcess* p, VitaThread* t, uint32_t ptr, uint32_t size, uint32_t alignment) {
    (void)t;
    return vita_heap_realloc(p, heap(p), ptr, size, alignment);
}
uint32_t vita_libc_lc_malloc_usable_size(VitaProcess* p, VitaThread* t, uint32_t ptr) {
    (void)t;
    return vita_heap_usable(p, heap(p), ptr);
}
uint32_t vita_libc_cxx_new(VitaProcess* p, VitaThread* t, uint32_t size) {
    uint32_t r = vita_heap_alloc(p, heap(p), size ? size : 1, 8);
    return r ? r : fatal(p, t, "operator new found no memory");
}
/* the nothrow overloads: same args plus a reference to the nothrow tag, which we never touch. 0 on failure instead
   of fatal, same as real nothrow new */
uint32_t vita_libc_cxx_new_nothrow(VitaProcess* p, VitaThread* t, uint32_t size) {
    (void)t;
    return vita_heap_alloc(p, heap(p), size ? size : 1, 8);
}
uint32_t vita_libc_cxx_delete(VitaProcess* p, VitaThread* t, uint32_t ptr) {
    (void)t;
    if (ptr) vita_heap_free(p, heap(p), ptr);
    return 0;
}
/* the sized-delete overloads (operator delete(void*, unsigned int)): the size is ignored, same free either way */
uint32_t vita_libc_cxx_delete_sized(VitaProcess* p, VitaThread* t, uint32_t ptr) {
    return vita_libc_cxx_delete(p, t, ptr);
}
/* the Clib's mspaces: a heap in the memory the game gives */
uint32_t vita_libc_clib_mspace_create(VitaProcess* p, VitaThread* t, uint32_t base, uint32_t size) {
    (void)t;
    return vita_heap_init(p, base, size);
}
uint32_t vita_libc_clib_mspace_malloc(VitaProcess* p, VitaThread* t, uint32_t heap, uint32_t size) {
    (void)t;
    return vita_heap_alloc(p, heap, size, 8);
}
uint32_t vita_libc_clib_mspace_calloc(VitaProcess* p, VitaThread* t, uint32_t heap, uint32_t nmemb,
                                      uint32_t member_size) {
    (void)t;
    uint64_t size = (uint64_t)nmemb * member_size;
    uint32_t r = size > UINT32_MAX ? 0 : vita_heap_alloc(p, heap, (uint32_t)size, 8);
    if (r && size) memset(host(p, r), 0, (size_t)size);
    return r;
}
uint32_t vita_libc_clib_mspace_memalign(VitaProcess* p, VitaThread* t, uint32_t heap, uint32_t align, uint32_t size) {
    (void)t;
    return vita_heap_alloc(p, heap, size, align > 8 ? align : 8);
}
uint32_t vita_libc_clib_mspace_realloc(VitaProcess* p, VitaThread* t, uint32_t heap, uint32_t ptr, uint32_t size) {
    (void)t;
    return vita_heap_realloc(p, heap, ptr, size, 8);
}
uint32_t vita_libc_clib_strnlen(VitaProcess* p, VitaThread* t, uint32_t string, uint32_t n) {
    (void)t;
    const char* s = host(p, string);
    uint32_t i = 0;
    while (i < n && s[i]) ++i;
    return i;
}
uint32_t vita_libc_clib_strlcpy(VitaProcess* p, VitaThread* t, uint32_t dst, uint32_t src, uint32_t n) {
    (void)t;
    char* d = host(p, dst);
    const char* s = host(p, src);
    uint32_t len = (uint32_t)strlen(s);
    if (n) {
        uint32_t c = len < n - 1 ? len : n - 1;
        memmove(d, s, c);
        d[c] = 0;
    }
    return len;
}
uint32_t vita_libc_clib_strlcat(VitaProcess* p, VitaThread* t, uint32_t dst, uint32_t src, uint32_t n) {
    (void)t;
    char* d = host(p, dst);
    const char* s = host(p, src);
    uint32_t dl = 0, len = (uint32_t)strlen(s);
    while (dl < n && d[dl]) ++dl;
    if (dl == n) return n + len;
    uint32_t c = len < n - dl - 1 ? len : n - dl - 1;
    memmove(d + dl, s, c);
    d[dl + c] = 0;
    return dl + len;
}
uint32_t vita_libc_clib_tolower(VitaProcess* p, VitaThread* t, uint32_t c) {
    (void)p, (void)t;
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
uint32_t vita_libc_clib_toupper(VitaProcess* p, VitaThread* t, uint32_t c) {
    (void)p, (void)t;
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}
uint32_t vita_libc_clib_mspace_free(VitaProcess* p, VitaThread* t, uint32_t heap, uint32_t ptr) {
    (void)t;
    vita_heap_free(p, heap, ptr);
    return 0;
}
uint32_t vita_libc_zero(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
uint32_t vita_libc_first_argument(VitaProcess* p, VitaThread* t, uint32_t self) {
    (void)p, (void)t;
    return self;
}

/* ---- memory and strings, in place ---- */
uint32_t vita_libc_lc_memcpy(VitaProcess* p, VitaThread* t, uint32_t d, uint32_t src, uint32_t n) {
    (void)t;
    if (n) memmove(host(p, d), host(p, src), n);
    return d;
}
uint32_t vita_libc_lc_memset(VitaProcess* p, VitaThread* t, uint32_t d, uint32_t c, uint32_t n) {
    (void)t;
    if (n) memset(host(p, d), (int)c, n);
    return d;
}
uint32_t vita_libc_lc_memcmp(VitaProcess* p, VitaThread* t, uint32_t s1, uint32_t s2, uint32_t n) {
    (void)t;
    return n ? (uint32_t)memcmp(host(p, s1), host(p, s2), n) : 0;
}
uint32_t vita_libc_lc_strlen(VitaProcess* p, VitaThread* t, uint32_t s) {
    (void)t;
    return (uint32_t)strlen(host(p, s));
}
uint32_t vita_libc_lc_strcmp(VitaProcess* p, VitaThread* t, uint32_t s1, uint32_t s2) {
    (void)t;
    return (uint32_t)strcmp(host(p, s1), host(p, s2));
}
uint32_t vita_libc_lc_strncmp(VitaProcess* p, VitaThread* t, uint32_t s1, uint32_t s2, uint32_t n) {
    (void)t;
    return (uint32_t)strncmp(host(p, s1), host(p, s2), n);
}
static int casecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}
uint32_t vita_libc_lc_strcasecmp(VitaProcess* p, VitaThread* t, uint32_t s1, uint32_t s2) {
    (void)t;
    return (uint32_t)casecmp(host(p, s1), host(p, s2), SIZE_MAX);
}
uint32_t vita_libc_lc_strncasecmp(VitaProcess* p, VitaThread* t, uint32_t s1, uint32_t s2, uint32_t n) {
    (void)t;
    return (uint32_t)casecmp(host(p, s1), host(p, s2), n);
}
uint32_t vita_libc_lc_strcpy(VitaProcess* p, VitaThread* t, uint32_t d, uint32_t src) {
    (void)t;
    const char* s = host(p, src);
    memmove(host(p, d), s, strlen(s) + 1);
    return d;
}
uint32_t vita_libc_lc_strncpy(VitaProcess* p, VitaThread* t, uint32_t d, uint32_t src, uint32_t n) {
    (void)t;
    if (n) strncpy(host(p, d), host(p, src), n);
    return d;
}
uint32_t vita_libc_lc_strcat(VitaProcess* p, VitaThread* t, uint32_t d, uint32_t src) {
    (void)t;
    const char* s = host(p, src);
    char* end = host(p, d) + strlen(host(p, d));
    memmove(end, s, strlen(s) + 1);
    return d;
}
uint32_t vita_libc_lc_strncat(VitaProcess* p, VitaThread* t, uint32_t d, uint32_t src, uint32_t n) {
    (void)t;
    const char* s = host(p, src);
    char* end = host(p, d) + strlen(host(p, d));
    size_t length = 0;
    while (length < n && s[length]) ++length;
    memmove(end, s, length);
    end[length] = 0;
    return d;
}
uint32_t vita_libc_lc_strchr(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t c) {
    (void)t;
    return guest(p, strchr(host(p, s), (int)(c & 0xff)));
}
uint32_t vita_libc_lc_memchr(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t c, uint32_t n) {
    (void)t;
    return n ? guest(p, memchr(host(p, s), (int)(c & 0xff), n)) : 0;
}
uint32_t vita_libc_lc_strrchr(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t c) {
    (void)t;
    return guest(p, strrchr(host(p, s), (int)(c & 0xff)));
}
uint32_t vita_libc_lc_strstr(VitaProcess* p, VitaThread* t, uint32_t haystack, uint32_t needle) {
    (void)t;
    return guest(p, strstr(host(p, haystack), host(p, needle)));
}
uint32_t vita_libc_lc_strspn(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t accept) {
    (void)t;
    return (uint32_t)strspn(host(p, s), host(p, accept));
}
uint32_t vita_libc_lc_strpbrk(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t accept) {
    (void)t;
    const char* at = strpbrk(host(p, s), host(p, accept));
    return at ? guest(p, at) : 0;
}
uint32_t vita_libc_lc_strcspn(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t reject) {
    (void)t;
    return (uint32_t)strcspn(host(p, s), host(p, reject));
}
/* the wide strings: 16-bit wchar_t */
static uint32_t wide_length(VitaProcess* p, uint32_t s) {
    uint32_t n = 0;
    while (vita_read16(p, s + n * 2)) ++n;
    return n;
}
uint32_t vita_libc_lc_wcslen(VitaProcess* p, VitaThread* t, uint32_t s) {
    (void)t;
    return wide_length(p, s);
}
static int wide_compare(VitaProcess* p, uint32_t a, uint32_t b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t x = vita_read16(p, a + i * 2), y = vita_read16(p, b + i * 2);
        if (x != y || !x) return x < y ? -1 : x > y;
    }
    return 0;
}
uint32_t vita_libc_lc_wcscmp(VitaProcess* p, VitaThread* t, uint32_t a, uint32_t b) {
    (void)t;
    return (uint32_t)wide_compare(p, a, b, UINT32_MAX);
}
uint32_t vita_libc_lc_wcsncmp(VitaProcess* p, VitaThread* t, uint32_t a, uint32_t b, uint32_t n) {
    (void)t;
    return (uint32_t)wide_compare(p, a, b, n);
}
uint32_t vita_libc_lc_wcschr(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t c) {
    (void)t;
    for (;; s += 2) {
        uint32_t u = vita_read16(p, s);
        if (u == c) return s;
        if (!u) return 0;
    }
}
uint32_t vita_libc_lc_wmemchr(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t c, uint32_t n) {
    (void)t;
    for (uint32_t i = 0; i < n; ++i)
        if (vita_read16(p, s + i * 2) == c) return s + i * 2;
    return 0;
}
uint32_t vita_libc_lc_wmemcpy(VitaProcess* p, VitaThread* t, uint32_t d, uint32_t src, uint32_t n) {
    (void)t;
    if (n) memmove(host(p, d), host(p, src), (size_t)n * 2);
    return d;
}
uint32_t vita_libc_lc_wmemset(VitaProcess* p, VitaThread* t, uint32_t d, uint32_t wc, uint32_t n) {
    (void)t;
    uint16_t c = (uint16_t)wc;
    for (uint32_t i = 0; i < n; ++i) memcpy(host(p, d + i * 2), &c, 2);
    return d;
}
uint32_t vita_libc_lc_wcsstr(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t w) {
    (void)t;
    uint32_t n = wide_length(p, w);
    for (;; s += 2) {
        if (!wide_compare(p, s, w, n)) return s;
        if (!vita_read16(p, s)) return 0;
    }
}
/* _Towctrans(c, which): wctrans's "tolower" (1) or "toupper" (2) */
uint32_t vita_libc_lc_towctrans(VitaProcess* p, VitaThread* t, uint32_t c, uint32_t which) {
    (void)p, (void)t;
    if (c >= 0x80) return c;
    return which == 1 ? (uint32_t)tolower((int)c) : which == 2 ? (uint32_t)toupper((int)c) : c;
}

/* ---- numbers from strings: the host's, on the guest's string ---- */
uint32_t vita_libc_lc_strtol(VitaProcess* p, VitaThread* t, uint32_t nptr, uint32_t endptr, uint32_t base) {
    (void)t;
    char *s = host(p, nptr), *end = s;
    long long v = strtoll(s, &end, (int)base);
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    if (endptr) vita_write32(p, endptr, guest(p, end));
    return (uint32_t)(int32_t)v;
}
uint32_t vita_libc_lc_stoul(VitaProcess* p, VitaThread* t, uint32_t nptr, uint32_t endptr, uint32_t base) {
    (void)t;
    char *s = host(p, nptr), *end = s;
    unsigned long long v = strtoull(s, &end, (int)base);
    if (endptr) vita_write32(p, endptr, guest(p, end));
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}
uint64_t vita_libc_lc_stoull(VitaProcess* p, VitaThread* t, uint32_t nptr, uint32_t endptr, uint32_t base) {
    (void)t;
    char *s = host(p, nptr), *end = s;
    unsigned long long v = strtoull(s, &end, (int)base);
    if (endptr) vita_write32(p, endptr, guest(p, end));
    return v;
}
uint64_t vita_libc_lc_stoll(VitaProcess* p, VitaThread* t, uint32_t nptr, uint32_t endptr, uint32_t base) {
    (void)t;
    char *s = host(p, nptr), *end = s;
    long long v = strtoll(s, &end, (int)base);
    if (endptr) vita_write32(p, endptr, guest(p, end));
    return (uint64_t)v;
}
/* _Stod(s, endptr, pten): the number times 10 to pten */
double vita_libc_lc_stod(VitaProcess* p, VitaThread* t, uint32_t string, uint32_t endptr, int32_t pten) {
    (void)t;
    char *s = host(p, string), *end = s;
    double v = strtod(s, &end);
    if (pten) v *= pow(10.0, pten);
    if (endptr) vita_write32(p, endptr, guest(p, end));
    return v;
}

/* ---- printf ---- */
typedef struct {
    VitaProcess* p;
    VitaThread* t;
    unsigned next; /* the next argument's word: in the registers and on the stack */
    uint32_t list; /* a va_list's next word instead, when set */
} Args;
static uint32_t next_word(Args* a) {
    if (a->list) {
        uint32_t v = vita_read32(a->p, a->list);
        a->list += 4;
        return v;
    }
    return vita_arg(a->p, a->t, a->next++);
}
static uint64_t next_dword(Args* a) {
    if (a->list)
        a->list = (a->list + 7) & ~7u;
    else
        a->next = (a->next + 1) & ~1u;
    uint64_t low = next_word(a);
    return low | (uint64_t)next_word(a) << 32;
}
/* what printf makes, as code units: bytes, or a wide printf's wchar_t */
typedef struct {
    uint16_t* units;
    size_t length, capacity;
} Out;
static void put(Out* o, uint32_t unit) {
    if (o->length == o->capacity) {
        size_t capacity = o->capacity ? o->capacity * 2 : 256;
        uint16_t* grown = realloc(o->units, capacity * sizeof *grown);
        if (!grown) return;
        o->units = grown, o->capacity = capacity;
    }
    o->units[o->length++] = (uint16_t)unit;
}
static void put_text(Out* o, const char* s) {
    while (*s) put(o, (unsigned char)*s++);
}
/* a field's width or precision, a digit more: counted to FIELD_MOST and held just past it, however many digits a
   format has (the texts made are 4200 bytes) */
#define FIELD_MOST 4096
static int field_digit(int value, uint32_t digit) {
    return value > FIELD_MOST ? value : value * 10 + (int)(digit - '0');
}
/* format's characters, wide (16-bit) ones or bytes */
static uint32_t unit_at(VitaProcess* p, uint32_t address, bool wide, uint32_t i) {
    return wide ? vita_read16(p, address + i * 2) : p->mem[(uint32_t)(address + i)];
}
static void format(Args* a, uint32_t fmt, bool wide, Out* o) {
    VitaProcess* p = a->p;
    for (uint32_t i = 0;;) {
        uint32_t c = unit_at(p, fmt, wide, i++);
        if (!c) return;
        if (c != '%') {
            put(o, c);
            continue;
        }
        char spec[32] = "%";
        size_t n = 1;
        bool left = false;
        for (c = unit_at(p, fmt, wide, i); c && strchr("-+ #0", (int)c); c = unit_at(p, fmt, wide, ++i)) {
            if (n < 8) spec[n++] = (char)c;
            if (c == '-') left = true;
        }
        /* a width and a precision are any digits, or any 32 bits a * takes: counted to FIELD_MOST and no further */
        int width = -1, precision = -1;
        if (c == '*') {
            width = (int32_t)next_word(a);
            if (width < 0) left = true, width = width < -FIELD_MOST ? FIELD_MOST : -width, spec[n++] = '-';
            c = unit_at(p, fmt, wide, ++i);
        } else if (c >= '0' && c <= '9') {
            for (width = 0; c >= '0' && c <= '9'; c = unit_at(p, fmt, wide, ++i)) width = field_digit(width, c);
        }
        if (c == '.') {
            c = unit_at(p, fmt, wide, ++i);
            precision = 0;
            if (c == '*') {
                precision = (int32_t)next_word(a);
                c = unit_at(p, fmt, wide, ++i);
            } else {
                for (; c >= '0' && c <= '9'; c = unit_at(p, fmt, wide, ++i)) precision = field_digit(precision, c);
            }
        }
        if (width > FIELD_MOST) width = FIELD_MOST;
        if (precision > FIELD_MOST) precision = FIELD_MOST;
        if (width >= 0) n += (size_t)snprintf(spec + n, sizeof spec - n, "%d", width);
        if (precision >= 0) n += (size_t)snprintf(spec + n, sizeof spec - n, ".%d", precision);
        int longs = 0, shorts = 0;
        for (;; c = unit_at(p, fmt, wide, ++i)) {
            if (c == 'l') ++longs;
            else if (c == 'h') ++shorts;
            else if (c == 'q' || c == 'j' || c == 'L') longs = 2;
            else if (c != 'z' && c != 't') break;
        }
        ++i;
        char text[4200];
        text[0] = 0;
        switch (c) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
                uint64_t v = longs >= 2 ? next_dword(a) : next_word(a);
                bool sign = c == 'd' || c == 'i';
                if (longs < 2) v = sign ? (uint64_t)(int64_t)(int32_t)v : (uint32_t)v;
                if (shorts == 1) v = sign ? (uint64_t)(int64_t)(int16_t)v : (uint16_t)v;
                if (shorts >= 2) v = sign ? (uint64_t)(int64_t)(int8_t)v : (uint8_t)v;
                spec[n++] = 'l', spec[n++] = 'l', spec[n++] = (char)c, spec[n] = 0;
                if (sign)
                    snprintf(text, sizeof text, spec, (long long)v);
                else
                    snprintf(text, sizeof text, spec, (unsigned long long)v);
                put_text(o, text);
                break;
            }
            case 'p':
                snprintf(text, sizeof text, "0x%08" PRIx32, next_word(a));
                put_text(o, text);
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                uint64_t bits = next_dword(a);
                double d;
                memcpy(&d, &bits, sizeof d);
                spec[n++] = (char)c, spec[n] = 0;
                snprintf(text, sizeof text, spec, d);
                put_text(o, text);
                break;
            }
            case 'c': case 'C': case 's': case 'S': {
                /* the units, then the padding */
                uint32_t units[1] = {0}, address = 0, count = 0;
                bool wide_arg = c == 'C' || c == 'S' || longs;
                if (c == 'c' || c == 'C') {
                    units[0] = next_word(a) & (wide_arg ? 0xffff : 0xff);
                    count = 1;
                } else {
                    address = next_word(a);
                    if (!address) {
                        address = 0;
                        count = 6;
                    } else {
                        while ((precision < 0 || count < (uint32_t)precision) && unit_at(p, address, wide_arg, count))
                            ++count;
                    }
                }
                int pad = width > (int)count ? width - (int)count : 0;
                for (int k = 0; !left && k < pad; ++k) put(o, ' ');
                if (c == 'c' || c == 'C')
                    put(o, units[0]);
                else if (!address)
                    put_text(o, "(null)");
                else
                    for (uint32_t k = 0; k < count; ++k) put(o, unit_at(p, address, wide_arg, k));
                for (int k = 0; left && k < pad; ++k) put(o, ' ');
                break;
            }
            case 'n': {
                uint32_t to = next_word(a);
                if (to && shorts) vita_write16(p, to, (uint32_t)o->length);
                else if (to) vita_write32(p, to, (uint32_t)o->length);
                break;
            }
            case '%': put(o, '%'); break;
            default: /* an unknown conversion, shown as written */
                put(o, '%');
                if (c) put(o, c);
                else return;
                break;
        }
    }
}
/* formatted into a narrow guest buffer of size bytes (none for size 0), NUL-terminated within it: the full length */
static uint32_t to_buffer(VitaProcess* p, uint32_t buffer, uint64_t size, const Out* o) {
    if (buffer && size) {
        size_t n = o->length < size ? o->length : (size_t)size - 1;
        for (size_t i = 0; i < n; ++i) p->mem[(uint32_t)(buffer + i)] = (unsigned char)o->units[i];
        p->mem[(uint32_t)(buffer + n)] = 0;
    }
    return (uint32_t)o->length;
}
/* the same into a wide buffer of size units: -1 when it didn't fit, as swprintf returns */
static uint32_t to_wide(VitaProcess* p, uint32_t buffer, uint32_t size, const Out* o) {
    if (!buffer || !size) return UINT32_MAX;
    size_t n = o->length < size ? o->length : size - 1;
    for (size_t i = 0; i < n; ++i) vita_write16(p, buffer + (uint32_t)i * 2, o->units[i]);
    vita_write16(p, buffer + (uint32_t)n * 2, 0);
    return o->length < size ? (uint32_t)o->length : UINT32_MAX;
}
static void to_host(FILE* f, const Out* o) {
    for (size_t i = 0; i < o->length; ++i) fputc(o->units[i] < 0x100 ? o->units[i] : '?', f);
    fflush(f);
}
/* each printf: the format's argument, then its first argument's (or the va_list's) */
/* format, its arguments the call's words from first on (the registers, then the stack), or a va_list's when list is
   set */
static Out print(VitaProcess* p, VitaThread* t, uint32_t fmt, unsigned first, uint32_t list, bool wide) {
    Args a = {p, t, first, list};
    Out o = {0};
    format(&a, fmt, wide, &o);
    return o;
}
uint32_t vita_libc_lc_printf(VitaProcess* p, VitaThread* t, uint32_t fmt) {
    Out o = print(p, t, fmt, 1, 0, false);
    to_host(stdout, &o);
    free(o.units);
    return (uint32_t)o.length;
}
uint32_t vita_libc_lc_sprintf(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t fmt) {
    Out o = print(p, t, fmt, 2, 0, false);
    uint32_t r = to_buffer(p, buffer, UINT64_MAX, &o);
    free(o.units);
    return r;
}
uint32_t vita_libc_lc_vsprintf(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t fmt, uint32_t list) {
    Out o = print(p, t, fmt, 2, list, false);
    uint32_t r = to_buffer(p, buffer, UINT64_MAX, &o);
    free(o.units);
    return r;
}
uint32_t vita_libc_lc_snprintf(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t size, uint32_t fmt) {
    Out o = print(p, t, fmt, 3, 0, false);
    uint32_t r = to_buffer(p, buffer, size, &o);
    free(o.units);
    return r;
}
uint32_t vita_libc_lc_vsnprintf(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t size, uint32_t fmt,
                                uint32_t list) {
    Out o = print(p, t, fmt, 3, list, false);
    uint32_t r = to_buffer(p, buffer, size, &o);
    free(o.units);
    return r;
}
uint32_t vita_libc_lc_swprintf(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t size, uint32_t fmt) {
    Out o = print(p, t, fmt, 3, 0, true);
    uint32_t r = to_wide(p, buffer, size, &o);
    free(o.units);
    return r;
}
uint32_t vita_libc_lc_vswprintf(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t size, uint32_t fmt,
                                uint32_t list) {
    Out o = print(p, t, fmt, 3, list, true);
    uint32_t r = to_wide(p, buffer, size, &o);
    free(o.units);
    return r;
}

/* ---- sscanf ---- */
uint32_t vita_libc_lc_sscanf(VitaProcess* p, VitaThread* t, uint32_t str, uint32_t fmt) {
    const char* in = host(p, str);
    uint32_t assigned = 0;
    Args a = {p, t, 2, 0};
    const char* s = in;
    bool any = false;
    for (uint32_t i = 0;;) {
        int c = p->mem[(uint32_t)(fmt + i++)];
        if (!c) break;
        if (isspace(c)) {
            while (isspace((unsigned char)*s)) ++s;
            continue;
        }
        if (c != '%' || p->mem[(uint32_t)(fmt + i)] == '%') {
            if (c == '%') ++i;
            if (*s != c) break;
            ++s;
            continue;
        }
        bool skip = false;
        int width = 0, longs = 0, shorts = 0;
        c = p->mem[(uint32_t)(fmt + i++)];
        if (c == '*') skip = true, c = p->mem[(uint32_t)(fmt + i++)];
        for (; c >= '0' && c <= '9'; c = p->mem[(uint32_t)(fmt + i++)]) width = field_digit(width, (uint32_t)c);
        for (;; c = p->mem[(uint32_t)(fmt + i++)]) {
            if (c == 'l') ++longs;
            else if (c == 'h') ++shorts;
            else if (c == 'L' || c == 'q' || c == 'j') longs = 2;
            else if (c != 'z' && c != 't') break;
        }
        if (c == 'n') {
            uint32_t to = skip ? 0 : next_word(&a);
            if (to) vita_write32(p, to, (uint32_t)(s - in));
            continue;
        }
        if (c != 'c' && c != '[') while (isspace((unsigned char)*s)) ++s;
        if (!*s) {
            if (!any) assigned = UINT32_MAX;
            break;
        }
        /* the field: at most width characters, copied */
        char field[512];
        size_t limit = width > 0 && (size_t)width < sizeof field ? (size_t)width : sizeof field - 1, n = 0;
        if (c == 'c') {
            size_t count = width > 0 ? (size_t)width : 1;
            uint32_t to = skip ? 0 : next_word(&a);
            for (size_t k = 0; k < count && s[k]; ++k)
                if (to) p->mem[(uint32_t)(to + k)] = (unsigned char)s[k];
            s += strnlen(s, count);
            if (!skip) ++assigned;
            any = true;
            continue;
        }
        if (c == 's' || c == '[') {
            bool set[256] = {false}, invert = false;
            if (c == '[') {
                if (p->mem[(uint32_t)(fmt + i)] == '^') invert = true, ++i;
                if (p->mem[(uint32_t)(fmt + i)] == ']') set[']'] = true, ++i;
                for (int ch; (ch = p->mem[(uint32_t)(fmt + i)]) && ch != ']'; ++i) {
                    if (p->mem[(uint32_t)(fmt + i + 1)] == '-' && p->mem[(uint32_t)(fmt + i + 2)] &&
                        p->mem[(uint32_t)(fmt + i + 2)] != ']') {
                        for (int k = ch; k <= p->mem[(uint32_t)(fmt + i + 2)]; ++k) set[k] = true;
                        i += 2;
                    } else
                        set[ch] = true;
                }
                if (p->mem[(uint32_t)(fmt + i)] == ']') ++i;
            }
            uint32_t to = skip ? 0 : next_word(&a);
            size_t most = width > 0 ? (size_t)width : SIZE_MAX;
            while (*s && n < most && (c == 's' ? !isspace((unsigned char)*s) : set[(unsigned char)*s] != invert)) {
                if (to) p->mem[(uint32_t)(to + n)] = (unsigned char)*s;
                ++s, ++n;
            }
            if (!n) break;
            if (to) p->mem[(uint32_t)(to + n)] = 0;
            if (!skip) ++assigned;
            any = true;
            continue;
        }
        n = strnlen(s, limit);
        memcpy(field, s, n);
        field[n] = 0;
        char* end = field;
        if (strchr("diuoxXp", c)) {
            int base = c == 'd' || c == 'u' ? 10 : c == 'o' ? 8 : c == 'i' ? 0 : 16;
            unsigned long long v = c == 'd' || c == 'i' ? (unsigned long long)strtoll(field, &end, base)
                                                        : strtoull(field, &end, base);
            if (end == field) break;
            uint32_t to = skip ? 0 : next_word(&a);
            if (to && longs >= 2) vita_write64(p, to, v);
            else if (to && shorts == 1) vita_write16(p, to, (uint32_t)v);
            else if (to && shorts >= 2) p->mem[to] = (unsigned char)v;
            else if (to) vita_write32(p, to, (uint32_t)v);
        } else if (strchr("feEgGaA", c)) {
            double v = strtod(field, &end);
            if (end == field) break;
            uint32_t to = skip ? 0 : next_word(&a);
            if (to && longs) {
                uint64_t bits;
                memcpy(&bits, &v, sizeof bits);
                vita_write64(p, to, bits);
            } else if (to) {
                float f = (float)v;
                uint32_t bits;
                memcpy(&bits, &f, sizeof bits);
                vita_write32(p, to, bits);
            }
        } else
            break;
        s += end - field;
        if (!skip) ++assigned;
        any = true;
    }
    return assigned;
}

/* ---- stdio: a FILE the runtime made (its magic, then the descriptor), _Stdout and _Stderr in their slots ---- */
static uint32_t file_fd(VitaProcess* p, uint32_t file) {
    return file && vita_mapped(p, file, 8) && vita_read32(p, file) == FILE_MAGIC ? vita_read32(p, file + 4) : 0;
}
static FILE* stream(VitaProcess* p, uint32_t file) {
    uint32_t fd = file_fd(p, file);
    return fd == STDOUT_FD ? stdout : fd == STDERR_FD ? stderr : fd ? vita_io_file(p, fd) : NULL;
}
static uint32_t open_stream(VitaProcess* p, VitaThread* t, uint32_t path, uint32_t flags);
uint32_t vita_libc_lc_fopen(VitaProcess* p, VitaThread* t, uint32_t path, uint32_t mode_string) {
    char mode[8];
    vita_string(p, mode_string, mode, sizeof mode);
    uint32_t flags = mode[0] == 'r' ? 0x0001u : mode[0] == 'w' ? 0x0002u | 0x0200u | 0x0400u
                     : mode[0] == 'a' ? 0x0002u | 0x0200u | 0x0100u : 0;
    if (strchr(mode, '+')) flags |= 0x0003u;
    if (!flags) return set_errno(p, t, EINVAL), 0;
    return open_stream(p, t, path, flags);
}
static uint32_t open_stream(VitaProcess* p, VitaThread* t, uint32_t path, uint32_t flags) {
    uint32_t fd = vita_io_file_open(p, path, flags);
    if (fd & 0x80000000u) return set_errno(p, t, fd & 0xffff), 0;
    uint32_t file = vita_heap_alloc(p, heap(p), 16, 8);
    if (!file) {
        vita_io_file_close(p, fd);
        return set_errno(p, t, ENOMEM), 0;
    }
    vita_write32(p, file, FILE_MAGIC), vita_write32(p, file + 4, fd);
    return file;
}
uint32_t vita_libc_lc_fclose(VitaProcess* p, VitaThread* t, uint32_t file) {
    (void)t;
    uint32_t fd = file_fd(p, file);
    if (!fd || fd == STDOUT_FD || fd == STDERR_FD) return fd ? 0 : UINT32_MAX;
    vita_io_file_close(p, fd);
    vita_write32(p, file, 0);
    vita_heap_free(p, heap(p), file);
    return 0;
}
uint32_t vita_libc_lc_fread(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t size, uint32_t count,
                            uint32_t file) {
    (void)t;
    FILE* f = stream(p, file);
    uint64_t bytes = (uint64_t)size * count; /* past 4 GiB for some sizes and counts: no buffer of a game's is */
    if (!f || !bytes || bytes > UINT32_MAX || !vita_mapped(p, buffer, (uint32_t)bytes)) return 0;
    vita_watch_release(buffer, (uint32_t)bytes); /* the system writes it (watch.h) */
    return (uint32_t)fread(host(p, buffer), size, count, f);
}
uint32_t vita_libc_lc_fwrite(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t size, uint32_t count,
                             uint32_t file) {
    (void)t;
    FILE* f = stream(p, file);
    uint64_t bytes = (uint64_t)size * count;
    if (!f || !bytes || bytes > UINT32_MAX || !vita_mapped(p, buffer, (uint32_t)bytes)) return 0;
    return (uint32_t)fwrite(host(p, buffer), size, count, f);
}
uint32_t vita_libc_lc_fseek(VitaProcess* p, VitaThread* t, uint32_t file, int32_t offset, uint32_t whence) {
    (void)t;
    FILE* f = stream(p, file);
    return f && !fseek(f, (long)offset, (int)whence) ? 0 : UINT32_MAX;
}
uint32_t vita_libc_lc_ftell(VitaProcess* p, VitaThread* t, uint32_t file) {
    (void)t;
    FILE* f = stream(p, file);
    return f ? (uint32_t)ftell(f) : UINT32_MAX;
}
uint32_t vita_libc_lc_fflush(VitaProcess* p, VitaThread* t, uint32_t file) {
    (void)t;
    FILE* f = file ? stream(p, file) : stdout;
    if (f) fflush(f);
    return 0;
}
uint32_t vita_libc_lc_fputs(VitaProcess* p, VitaThread* t, uint32_t s, uint32_t file) {
    (void)t;
    FILE* f = stream(p, file);
    if (!f) return UINT32_MAX;
    fputs(host(p, s), f);
    if (f == stdout || f == stderr) fflush(f);
    return 0;
}
uint32_t vita_libc_lc_fprintf(VitaProcess* p, VitaThread* t, uint32_t file, uint32_t fmt) {
    FILE* f = stream(p, file);
    Out o = print(p, t, fmt, 2, 0, false);
    if (f) to_host(f, &o);
    free(o.units);
    return f ? (uint32_t)o.length : UINT32_MAX;
}
static void stdout_init(VitaProcess* p, uint32_t slot) {
    vita_write32(p, slot, FILE_MAGIC), vita_write32(p, slot + 4, STDOUT_FD);
}
static void stderr_init(VitaProcess* p, uint32_t slot) {
    vita_write32(p, slot, FILE_MAGIC), vita_write32(p, slot + 4, STDERR_FD);
}
/* the ctype tables' pointers, at their entry for 0 (EOF's before it) */
static void ctype_init(VitaProcess* p, uint32_t slot) { vita_write32(p, slot, data(p) + CTYPE_TABLE + 2); }
static void tolower_init(VitaProcess* p, uint32_t slot) { vita_write32(p, slot, data(p) + LOWER_TABLE + 2); }
static void toupper_init(VitaProcess* p, uint32_t slot) { vita_write32(p, slot, data(p) + UPPER_TABLE + 2); }
static void float_inf_init(VitaProcess* p, uint32_t slot) { vita_write32(p, slot, 0x7f800000u); }
static void float_nan_init(VitaProcess* p, uint32_t slot) { vita_write32(p, slot, 0x7fc00000u); }
static void double_inf_init(VitaProcess* p, uint32_t slot) { vita_write32(p, slot + 4, 0x7ff00000u); }
static void double_nan_init(VitaProcess* p, uint32_t slot) { vita_write32(p, slot + 4, 0x7ff80000u); }
/* an exception class's vtable: offset to top, type info, then functions that do nothing */
static void vtable_init(VitaProcess* p, uint32_t slot) {
    for (uint32_t i = 2; i < 8; ++i) vita_write32(p, slot + i * 4, VITA_RETURN);
}

/* ---- the process, setjmp, TLS, errno ---- */
uint32_t vita_libc_lc_exit(VitaProcess* p, VitaThread* t, int32_t exit_code) {
    (void)t;
    p->exited = true;
    p->exit_code = exit_code;
    return 0;
}
uint32_t vita_libc_lc_abort(VitaProcess* p, VitaThread* t) { return fatal(p, t, "abort()"); }
/* setjmp(env): r4 to r11, sp and lr, then d8 to d15, as only longjmp reads them */
uint32_t vita_libc_lc_setjmp(VitaProcess* p, VitaThread* t, uint32_t env) {
    for (unsigned i = 0; i < 10; ++i) vita_write32(p, env + i * 4, t->cpu.r[4 + i]);
    for (unsigned i = 0; i < 8; ++i) vita_write64(p, env + 40 + i * 8, t->cpu.d[8 + i]);
    return 0;
}
uint32_t vita_libc_lc_longjmp(VitaProcess* p, VitaThread* t, uint32_t env, uint32_t value) {
    for (unsigned i = 0; i < 10; ++i) t->cpu.r[4 + i] = vita_read32(p, env + i * 4);
    for (unsigned i = 0; i < 8; ++i)
        t->cpu.d[8 + i] = vita_read32(p, env + 40 + i * 8) | (uint64_t)vita_read32(p, env + 44 + i * 8) << 32;
    return value ? value : 1; /* returned from setjmp's call, by the stub's bx lr */
}
/* __tls_get_addr(&{module, offset}): the thread's copy of the module's thread-local data, made at its first use */
uint32_t vita_libc_lc_tls_get_addr(VitaProcess* p, VitaThread* t, uint32_t list) {
    const VitaImage* image = &p->image;
    if (!t->module_tls) {
        uint32_t block = image->tls_memsz ? vita_alloc(p, &p->user, image->tls_memsz, PAGE) : 0;
        if (!block) return fatal(p, t, "no thread-local data for a thread");
        memcpy(host(p, block), host(p, image->tls_image), image->tls_filesz);
        t->module_tls = block;
    }
    return t->module_tls + vita_read32(p, list + 4);
}
uint32_t vita_libc_lc_errno_loc(VitaProcess* p, VitaThread* t) {
    (void)p;
    return t->tls + VITA_TLS_ERRNO;
}
/* __cxa_guard_acquire(guard): 1 when the object is to be made (its guard's first byte clear), one thread at a time */
uint32_t vita_libc_cxa_guard_acquire(VitaProcess* p, VitaThread* t, uint32_t guard) {
    (void)t;
    return !(p->mem[guard] & 1);
}
uint32_t vita_libc_cxa_guard_release(VitaProcess* p, VitaThread* t, uint32_t guard) {
    (void)t;
    p->mem[guard] = 1;
    return 0;
}
uint32_t vita_libc_lc_rand(VitaProcess* p, VitaThread* t) {
    (void)t;
    p->libc.rand = p->libc.rand * 1103515245u + 12345u;
    return (p->libc.rand >> 16) & 0x7fff;
}
uint32_t vita_libc_lc_srand(VitaProcess* p, VitaThread* t, uint32_t seed) {
    (void)t;
    p->libc.rand = seed;
    return 0;
}
/* ldiv(n, d): its ldiv_t returned in memory, the address in r0 */
uint32_t vita_libc_lc_ldiv(VitaProcess* p, VitaThread* t, uint32_t out, int32_t n, int32_t d) {
    (void)t;
    int32_t q = d ? (d == -1 ? (int32_t)(0u - (uint32_t)n) : n / d) : 0, r = d ? (int32_t)((uint32_t)n - (uint32_t)q * (uint32_t)d) : 0;
    vita_write32(p, out, (uint32_t)q), vita_write32(p, out + 4, (uint32_t)r);
    return out;
}
/* time(t): a 32-bit time_t, as SCE's libc has it */
uint32_t vita_libc_lc_time(VitaProcess* p, VitaThread* t, uint32_t tloc) {
    (void)t;
    uint32_t now = (uint32_t)time(NULL);
    if (tloc) vita_write32(p, tloc, now);
    return now;
}
uint32_t vita_libc_lc_gmtime(VitaProcess* p, VitaThread* t, uint32_t at) {
    (void)t;
    uint32_t d = data(p);
    time_t when = (time_t)(int32_t)vita_read32(p, at);
    struct tm tm;
#ifdef _WIN32
    if (!d || gmtime_s(&tm, &when)) return 0;
#else
    if (!d || !gmtime_r(&when, &tm)) return 0;
#endif
    const int fields[9] = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_wday, tm.tm_yday,
                           tm.tm_isdst};
    for (unsigned i = 0; i < 9; ++i) vita_write32(p, d + TM_RESULT + i * 4, (uint32_t)fields[i]);
    return d + TM_RESULT;
}

/* ---- SceLibm: the hard-float ABI's, which hookgen's adapters keep: x (and y) from S0 and S1 or D0 and D1, the
   result to S0 or D0 ---- */
#define F1(name, expr)                                                                                                 \
    float name(VitaProcess* p, VitaThread* t, float x) {                                                               \
        (void)p, (void)t;                                                                                              \
        return expr;                                                                                                   \
    }
#define F2(name, expr)                                                                                                 \
    float name(VitaProcess* p, VitaThread* t, float x, float y) {                                                      \
        (void)p, (void)t;                                                                                              \
        return expr;                                                                                                   \
    }
#define D1(name, expr)                                                                                                 \
    double name(VitaProcess* p, VitaThread* t, double x) {                                                             \
        (void)p, (void)t;                                                                                              \
        return expr;                                                                                                   \
    }
#define D2(name, expr)                                                                                                 \
    double name(VitaProcess* p, VitaThread* t, double x, double y) {                                                   \
        (void)p, (void)t;                                                                                              \
        return expr;                                                                                                   \
    }
F1(vita_libc_lm_acosf, acosf(x))
F1(vita_libc_lm_asinf, asinf(x))
F1(vita_libc_lm_atanf, atanf(x))
F2(vita_libc_lm_atan2f, atan2f(x, y))
F1(vita_libc_lm_tanf, tanf(x))
F1(vita_libc_lm_expf, expf(x))
F2(vita_libc_lm_powf, powf(x, y))
F2(vita_libc_lm_fmodf, fmodf(x, y))
F1(vita_libc_lm_ceilf, ceilf(x))
F1(vita_libc_lm_floorf, floorf(x))
F1(vita_libc_lm_fabsf, fabsf(x))
F1(vita_libc_lm_sinf, sinf(x))
F1(vita_libc_lm_cosf, cosf(x))
F1(vita_libc_lm_sqrtf, sqrtf(x))
F1(vita_libc_lm_logf, logf(x))
D2(vita_libc_lm_pow, pow(x, y))
D2(vita_libc_lm_fmod, fmod(x, y))
D1(vita_libc_lm_exp, exp(x))
D1(vita_libc_lm_sqrt, sqrt(x))
D1(vita_libc_lm_floor, floor(x))
D1(vita_libc_lm_ceil, ceil(x))
float vita_libc_lm_ldexpf(VitaProcess* p, VitaThread* t, float x, int32_t exponent) {
    (void)p, (void)t;
    return ldexpf(x, exponent);
}
/* modff(x, float* integer) */
float vita_libc_lm_modff(VitaProcess* p, VitaThread* t, float x, uint32_t integer) {
    (void)t;
    float whole, part = modff(x, &whole);
    uint32_t bits;
    memcpy(&bits, &whole, sizeof bits);
    if (integer) vita_write32(p, integer, bits);
    return part;
}
/* Dinkumware's _FSin(x, quadrant offset): sin(x + offset * pi/2), cos for offset 1 */
float vita_libc_lm_fsin(VitaProcess* p, VitaThread* t, float x, uint32_t offset) {
    (void)p, (void)t;
    switch (offset & 3) {
        case 0: return sinf(x);
        case 1: return cosf(x);
        case 2: return -sinf(x);
        default: return -cosf(x);
    }
}
/* _FLog(x, base): 0 the natural, 1 base 10, 2 base 2 */
float vita_libc_lm_flog(VitaProcess* p, VitaThread* t, float x, uint32_t base) {
    (void)p, (void)t;
    return base == 1 ? log10f(x) : base == 2 ? log2f(x) : logf(x);
}
/* _FDtest(float* x): its class, Dinkumware's codes */
uint32_t vita_libc_lm_fdtest(VitaProcess* p, VitaThread* t, uint32_t list) {
    (void)t;
    uint32_t bits = vita_read32(p, list);
    float x;
    memcpy(&x, &bits, sizeof x);
    switch (fpclassify(x)) {
        case FP_NAN: return 2;
        case FP_INFINITE: return 1;
        case FP_ZERO: return 0;
        case FP_SUBNORMAL: return (uint32_t)-2;
        default: return (uint32_t)-1;
    }
}

/* the double libm's others, which a game's own printf and string-to-number code reach through Dinkumware's helpers */
D1(vita_libc_lm_fabs, fabs(x))
D1(vita_libc_lm_sin, sin(x))
D1(vita_libc_lm_cos, cos(x))
D1(vita_libc_lm_tan, tan(x))
D1(vita_libc_lm_asin, asin(x))
D1(vita_libc_lm_acos, acos(x))
D1(vita_libc_lm_atan, atan(x))
D2(vita_libc_lm_atan2, atan2(x, y))
D1(vita_libc_lm_log, log(x))
D1(vita_libc_lm_log10, log10(x))
D1(vita_libc_lm_log2, log2(x))
D1(vita_libc_lm_exp2, exp2(x))
D1(vita_libc_lm_trunc, trunc(x))
D1(vita_libc_lm_round, round(x))
D1(vita_libc_lm_rint, rint(x))
D1(vita_libc_lm_nearbyint, nearbyint(x))
D1(vita_libc_lm_cbrt, cbrt(x))
D1(vita_libc_lm_sinh, sinh(x))
D1(vita_libc_lm_cosh, cosh(x))
D1(vita_libc_lm_tanh, tanh(x))
D2(vita_libc_lm_hypot, hypot(x, y))
D2(vita_libc_lm_copysign, copysign(x, y))
D2(vita_libc_lm_fmax, fmax(x, y))
D2(vita_libc_lm_fmin, fmin(x, y))
F1(vita_libc_lm_truncf, truncf(x))
F1(vita_libc_lm_roundf, roundf(x))
F1(vita_libc_lm_rintf, rintf(x))
F1(vita_libc_lm_nearbyintf, nearbyintf(x))
F1(vita_libc_lm_log10f, log10f(x))
F1(vita_libc_lm_log2f, log2f(x))
F1(vita_libc_lm_exp2f, exp2f(x))
F1(vita_libc_lm_cbrtf, cbrtf(x))
F1(vita_libc_lm_sinhf, sinhf(x))
F1(vita_libc_lm_coshf, coshf(x))
F1(vita_libc_lm_tanhf, tanhf(x))
F2(vita_libc_lm_hypotf, hypotf(x, y))
F2(vita_libc_lm_copysignf, copysignf(x, y))
F2(vita_libc_lm_fmaxf, fmaxf(x, y))
F2(vita_libc_lm_fminf, fminf(x, y))
/* ldexp(x, int exponent) and frexp(x, int* exponent): the double in D0, the int in R0 */
double vita_libc_lm_ldexp(VitaProcess* p, VitaThread* t, double x, int32_t exponent) {
    (void)p, (void)t;
    return ldexp(x, exponent);
}
double vita_libc_lm_frexp(VitaProcess* p, VitaThread* t, double x, uint32_t out) {
    (void)t;
    int exponent = 0;
    double mantissa = frexp(x, &exponent);
    if (out && vita_mapped(p, out, 4)) vita_write32(p, out, (uint32_t)exponent);
    return mantissa;
}
/* modf(x, double* integer): the fraction in D0, the integer part through R0 */
double vita_libc_lm_modf(VitaProcess* p, VitaThread* t, double x, uint32_t out) {
    (void)t;
    double whole, part = modf(x, &whole);
    uint32_t words[2];
    memcpy(words, &whole, sizeof words);
    if (out && vita_mapped(p, out, 8)) vita_write32(p, out, words[0]), vita_write32(p, out + 4, words[1]);
    return part;
}
/* Dinkumware's classes: _Dtest(double* x), _FDsign(float x) and _Dsign(double x), the sign as the sign bit of the
   16-bit word it is in (0x8000) */
static int dinkum_class(double x) {
    switch (fpclassify(x)) {
        case FP_NAN: return 2;
        case FP_INFINITE: return 1;
        case FP_ZERO: return 0;
        case FP_SUBNORMAL: return -2;
        default: return -1;
    }
}
uint32_t vita_libc_lm_dtest(VitaProcess* p, VitaThread* t, uint32_t at) {
    (void)t;
    if (!at || !vita_mapped(p, at, 8)) return 0;
    uint32_t words[2] = {vita_read32(p, at), vita_read32(p, at + 4)};
    double x;
    memcpy(&x, words, sizeof x);
    return (uint32_t)dinkum_class(x);
}
uint32_t vita_libc_lm_dsign(VitaProcess* p, VitaThread* t, double x) {
    (void)p, (void)t;
    return signbit(x) ? 0x8000u : 0;
}
uint32_t vita_libc_lm_fdsign(VitaProcess* p, VitaThread* t, float x) {
    (void)p, (void)t;
    return signbit(x) ? 0x8000u : 0;
}
/* _Stolx(const char* s, char** end, int base, int* error): strtol, the error set when the value is out of range */
uint32_t vita_libc_lc_stolx(VitaProcess* p, VitaThread* t, uint32_t nptr, uint32_t endptr, uint32_t base,
                            uint32_t error) {
    (void)t;
    char *s = host(p, nptr), *end = s;
    long long v = strtoll(s, &end, (int)base);
    if (endptr) vita_write32(p, endptr, guest(p, end));
    if (v > INT32_MAX || v < INT32_MIN) {
        if (error && vita_mapped(p, error, 4)) vita_write32(p, error, 1);
        return v > 0 ? (uint32_t)INT32_MAX : (uint32_t)INT32_MIN;
    }
    return (uint32_t)(int32_t)v;
}

/* _Fiopen(path, openmode, protection): what ifstream and ofstream open with. The openmode's bits: in 1, out 2, ate 4,
   app 8, trunc 0x10, binary 0x20 */
uint32_t vita_libc_cxx_fiopen(VitaProcess* p, VitaThread* t, uint32_t path, uint32_t mode) {
    uint32_t flags = 0;
    if (mode & 1) flags |= 0x0001u;
    if (mode & 2) flags |= 0x0002u;
    if ((mode & 3) == 2 || (mode & 0x10)) flags |= 0x0200u | 0x0400u; /* out alone makes the file and empties it */
    if (mode & 8) flags = (flags & ~0x0400u) | 0x0002u | 0x0200u | 0x0100u;
    if (!flags) return set_errno(p, t, EINVAL), 0;
    return open_stream(p, t, path, flags);
}
/* wide strings are 16-bit on the Vita */
uint32_t vita_libc_lc_wmemcmp(VitaProcess* p, VitaThread* t, uint32_t a, uint32_t b, uint32_t n) {
    (void)t;
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t x, y;
        memcpy(&x, host(p, a + i * 2), 2), memcpy(&y, host(p, b + i * 2), 2);
        if (x != y) return x < y ? UINT32_MAX : 1;
    }
    return 0;
}
uint32_t vita_libc_lc_labs(VitaProcess* p, VitaThread* t, int32_t v) {
    (void)p, (void)t;
    return v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
}
uint64_t vita_libc_lc_imaxabs(VitaProcess* p, VitaThread* t, int64_t v) {
    (void)p, (void)t;
    return v < 0 ? 0u - (uint64_t)v : (uint64_t)v;
}
/* _Log(x, base) as _FLog's, in double */
double vita_libc_lm_log_base(VitaProcess* p, VitaThread* t, double x, uint32_t base) {
    (void)p, (void)t;
    return base == 1 ? log10(x) : base == 2 ? log2(x) : log(x);
}

/* ---- SceLibstdcxx: what the game's code throws ends it ---- */
uint32_t vita_libc_cxx_length_error(VitaProcess* p, VitaThread* t) { return fatal(p, t, "std::length_error thrown"); }
uint32_t vita_libc_cxx_out_of_range(VitaProcess* p, VitaThread* t) { return fatal(p, t, "std::out_of_range thrown"); }
uint32_t vita_libc_cxx_raise(VitaProcess* p, VitaThread* t) { return fatal(p, t, "a C++ exception thrown"); }
/* type_info compares by identity, then by the name the second word points to */
/* the name of a type_info: the string its second word points to, or else (Dinkumware's type_info has a data word, then
   the name's characters inline) the bytes after that word. The layout is a guess: type_info::name logs the first
   objects' words, to check it */
static uint32_t typeinfo_name(VitaProcess* p, uint32_t info) {
    if (!info || !vita_mapped(p, info, 12)) return 0;
    uint32_t pointer = vita_read32(p, info + 4);
    if (pointer && vita_mapped(p, pointer, 1) && (isalnum(p->mem[pointer]) || p->mem[pointer] == '_')) return pointer;
    return info + 8;
}
static int typeinfo_same(VitaProcess* p, uint32_t a, uint32_t b) {
    if (a == b) return 1;
    uint32_t x = typeinfo_name(p, a), y = typeinfo_name(p, b);
    return x && y && (x == y || !strcmp(host(p, x), host(p, y)));
}
uint32_t vita_libc_cxx_typeinfo_name(VitaProcess* p, VitaThread* t, uint32_t info) {
    (void)t;
    static unsigned shown;
    if (shown < 3 && info && vita_mapped(p, info, 16)) {
        ++shown;
        printf("vita: type_info::name(0x%08" PRIx32 ") words %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
               info, vita_read32(p, info), vita_read32(p, info + 4), vita_read32(p, info + 8),
               vita_read32(p, info + 12));
    }
    return typeinfo_name(p, info);
}
uint32_t vita_libc_cxx_typeinfo_eq(VitaProcess* p, VitaThread* t, uint32_t a, uint32_t b) {
    (void)t;
    return (uint32_t)typeinfo_same(p, a, b);
}
uint32_t vita_libc_cxx_typeinfo_ne(VitaProcess* p, VitaThread* t, uint32_t a, uint32_t b) {
    (void)t;
    return (uint32_t)!typeinfo_same(p, a, b);
}
uint32_t vita_libc_cxa_allocate_exception(VitaProcess* p, VitaThread* t, uint32_t size) {
    uint32_t r = vita_heap_alloc(p, heap(p), size + 128, 8);
    return r ? r : fatal(p, t, "no memory for an exception");
}
/* ios_base::clear(state, reraise). The game's inline code reads the state at +8 and the rdbuf at +0x24, so the
   exception mask is taken as the word after the state. The first calls are logged to check it */
uint32_t vita_libc_ios_clear(VitaProcess* p, VitaThread* t, uint32_t self, uint32_t state) {
    static unsigned shown;
    if (shown++ < 4)
        printf("vita: ios_base::clear(0x%08" PRIx32 ", 0x%" PRIx32 ") words %08" PRIx32 " %08" PRIx32 " %08" PRIx32
               " %08" PRIx32 " rdbuf %08" PRIx32 "\n",
               self, state, vita_read32(p, self), vita_read32(p, self + 4), vita_read32(p, self + 8),
               vita_read32(p, self + 12), vita_read32(p, self + 0x24));
    vita_write32(p, self + 8, state & 0x17);
    if (state & vita_read32(p, self + 12) & 0x17) return fatal(p, t, "std::ios_base::failure thrown");
    return 0;
}
/* ios_base::_Init(this) gives the stream the state of a new one: good, no exceptions, skipws and dec, precision 6 */
uint32_t vita_libc_ios_init(VitaProcess* p, VitaThread* t, uint32_t self) {
    (void)t;
    vita_write32(p, self + 8, 0);
    vita_write32(p, self + 12, 0);
    vita_write32(p, self + 16, 0x201);
    vita_write32(p, self + 20, 6);
    vita_write32(p, self + 24, 0);
    return self;
}
/* std::exception::exception(this): its vtable one whose functions do nothing */
uint32_t vita_libc_cxx_exception(VitaProcess* p, VitaThread* t, uint32_t self) {
    (void)t;
    uint32_t d = data(p);
    if (d) vita_write32(p, self, d + EMPTY_VTABLE + 8);
    return self;
}

/* ---- the Clib's printf ---- */
uint32_t vita_libc_clib_snprintf(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t size, uint32_t fmt) {
    return vita_libc_lc_snprintf(p, t, buffer, size, fmt);
}
uint32_t vita_libc_clib_vsnprintf(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t size, uint32_t fmt,
                                  uint32_t list) {
    return vita_libc_lc_vsnprintf(p, t, buffer, size, fmt, list);
}

/* qsort(base, count, size, compare): the game's comparator runs as guest code, so the sort is an insertion sort
   (adjacent swaps) stepping one comparison per callback */
typedef struct {
    uint32_t base, count, size, compare, i, j;
} Qsort;
static uint32_t qsort_step(VitaProcess* p, VitaThread* t, Qsort* q);
static uint32_t qsort_compared(VitaProcess* p, VitaThread* t, void* context) {
    Qsort* q = context;
    if ((int32_t)t->cpu.r[0] > 0) {
        uint8_t* a = (uint8_t*)vita_ptr(p, q->base + (q->j - 1) * q->size);
        uint8_t* b = (uint8_t*)vita_ptr(p, q->base + q->j * q->size);
        for (uint32_t k = 0; k < q->size; ++k) {
            uint8_t x = a[k];
            a[k] = b[k];
            b[k] = x;
        }
        if (--q->j) return qsort_step(p, t, q);
    }
    ++q->i;
    q->j = q->i;
    return qsort_step(p, t, q);
}
static uint32_t qsort_step(VitaProcess* p, VitaThread* t, Qsort* q) {
    if (q->i >= q->count) {
        free(q);
        return 0;
    }
    const uint32_t args[2] = {q->base + (q->j - 1) * q->size, q->base + q->j * q->size};
    vita_hle_guest_call(p, t, q->compare, args, 2, qsort_compared, q);
    return 0;
}
uint32_t vita_libc_lc_qsort(VitaProcess* p, VitaThread* t, uint32_t base, uint32_t count, uint32_t size,
                            uint32_t compare) {
    if (count < 2 || !size || !compare) return 0;
    Qsort* q = calloc(1, sizeof *q);
    if (!q) return 0;
    q->base = base, q->count = count, q->size = size, q->compare = compare;
    q->i = q->j = 1;
    return qsort_step(p, t, q);
}

/* bsearch(key, base, count, size, compare): a binary search, one comparison of the game's comparator a callback; the
   element found, or 0 */
typedef struct {
    uint32_t key, base, size, compare, low, high;
} Bsearch;
static uint32_t bsearch_step(VitaProcess* p, VitaThread* t, Bsearch* b);
static uint32_t bsearch_compared(VitaProcess* p, VitaThread* t, void* context) {
    Bsearch* b = context;
    int32_t order = (int32_t)t->cpu.r[0];
    uint32_t middle = b->low + (b->high - b->low) / 2;
    if (!order) {
        uint32_t found = b->base + middle * b->size;
        free(b);
        return found;
    }
    if (order < 0)
        b->high = middle;
    else
        b->low = middle + 1;
    return bsearch_step(p, t, b);
}
static uint32_t bsearch_step(VitaProcess* p, VitaThread* t, Bsearch* b) {
    if (b->low >= b->high) {
        free(b);
        return 0;
    }
    const uint32_t args[2] = {b->key, b->base + (b->low + (b->high - b->low) / 2) * b->size};
    vita_hle_guest_call(p, t, b->compare, args, 2, bsearch_compared, b);
    return 0;
}
uint32_t vita_libc_lc_bsearch(VitaProcess* p, VitaThread* t, uint32_t key, uint32_t base, uint32_t count, uint32_t size,
                              uint32_t compare) {
    if (!count || !size || !compare) return 0;
    Bsearch* b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->key = key, b->base = base, b->size = size, b->compare = compare;
    b->high = count;
    return bsearch_step(p, t, b);
}

const VitaHleVariable vita_hle_libc_variables[] = {
    {0x5d8c1282u, "_Stdout", stdout_init},
    {0xddf9bb09u, "_Stderr", stderr_init},
    {0x3ce6109du, "_Ctype", ctype_init},
    {0xd662e07cu, "_Tolotab", tolower_init},
    {0x36878958u, "_Touptab", toupper_init},
    {0x8a0f308cu, "_FInf", float_inf_init},
    {0x415162ddu, "_FNan", float_nan_init},
    {0x710b5f33u, "_Inf", double_inf_init},
    {0x5289bbc0u, "_LInf", double_inf_init},
    {0x116f3da9u, "_Nan", double_nan_init},
    {0x036d0f07u, "_LNan", double_nan_init},
    {0x064ada35u, "_ZTVSt12length_error", vtable_init},
    {0x82a84e5eu, "_ZTVSt11logic_error", vtable_init},
    {0xddae7cbeu, "_ZTVSt12out_of_range", vtable_init},
    {0, NULL, NULL},
};
