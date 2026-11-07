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

static uint32_t arg(VitaProcess* p, VitaThread* t, unsigned n) { return vita_arg(p, t, n); }
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
static uint32_t lc_malloc(VitaProcess* p, VitaThread* t) {
    uint32_t r = vita_heap_alloc(p, heap(p), arg(p, t, 0), 8);
    if (!r) set_errno(p, t, ENOMEM);
    return r;
}
static uint32_t lc_calloc(VitaProcess* p, VitaThread* t) {
    uint64_t size = (uint64_t)arg(p, t, 0) * arg(p, t, 1);
    uint32_t r = size >> 31 ? 0 : vita_heap_alloc(p, heap(p), (uint32_t)size, 8);
    if (r) memset(host(p, r), 0, (size_t)size);
    return r;
}
static uint32_t lc_free(VitaProcess* p, VitaThread* t) {
    vita_heap_free(p, heap(p), arg(p, t, 0));
    return 0;
}
static uint32_t lc_realloc(VitaProcess* p, VitaThread* t) {
    uint32_t r = vita_heap_realloc(p, heap(p), arg(p, t, 0), arg(p, t, 1), 8);
    if (!r && arg(p, t, 1)) set_errno(p, t, ENOMEM);
    return r;
}
static uint32_t lc_memalign(VitaProcess* p, VitaThread* t) {
    return vita_heap_alloc(p, heap(p), arg(p, t, 1), arg(p, t, 0));
}
/* reallocalign(ptr, size, alignment) */
static uint32_t lc_reallocalign(VitaProcess* p, VitaThread* t) {
    return vita_heap_realloc(p, heap(p), arg(p, t, 0), arg(p, t, 1), arg(p, t, 2));
}
static uint32_t lc_malloc_usable_size(VitaProcess* p, VitaThread* t) {
    return vita_heap_usable(p, heap(p), arg(p, t, 0));
}
static uint32_t cxx_new(VitaProcess* p, VitaThread* t) {
    uint32_t size = arg(p, t, 0), r = vita_heap_alloc(p, heap(p), size ? size : 1, 8);
    return r ? r : fatal(p, t, "operator new found no memory");
}
/* the nothrow overloads: same args plus a reference to the nothrow tag, which we never touch. 0 on failure instead
   of fatal, same as real nothrow new */
static uint32_t cxx_new_nothrow(VitaProcess* p, VitaThread* t) {
    uint32_t size = arg(p, t, 0);
    return vita_heap_alloc(p, heap(p), size ? size : 1, 8);
}
static uint32_t cxx_delete(VitaProcess* p, VitaThread* t) {
    uint32_t ptr = arg(p, t, 0);
    if (ptr) vita_heap_free(p, heap(p), ptr);
    return 0;
}
/* the sized-delete overloads (operator delete(void*, unsigned int)): the size is ignored, same free either way */
static uint32_t cxx_delete_sized(VitaProcess* p, VitaThread* t) { return cxx_delete(p, t); }
/* the Clib's mspaces: a heap in the memory the game gives */
static uint32_t clib_mspace_create(VitaProcess* p, VitaThread* t) {
    return vita_heap_init(p, arg(p, t, 0), arg(p, t, 1));
}
static uint32_t clib_mspace_malloc(VitaProcess* p, VitaThread* t) {
    return vita_heap_alloc(p, arg(p, t, 0), arg(p, t, 1), 8);
}
static uint32_t clib_mspace_calloc(VitaProcess* p, VitaThread* t) {
    uint64_t size = (uint64_t)arg(p, t, 1) * arg(p, t, 2);
    uint32_t r = size > UINT32_MAX ? 0 : vita_heap_alloc(p, arg(p, t, 0), (uint32_t)size, 8);
    if (r && size) memset(host(p, r), 0, (size_t)size);
    return r;
}
static uint32_t clib_mspace_memalign(VitaProcess* p, VitaThread* t) {
    return vita_heap_alloc(p, arg(p, t, 0), arg(p, t, 2), arg(p, t, 1) > 8 ? arg(p, t, 1) : 8);
}
static uint32_t clib_mspace_realloc(VitaProcess* p, VitaThread* t) {
    return vita_heap_realloc(p, arg(p, t, 0), arg(p, t, 1), arg(p, t, 2), 8);
}
static uint32_t clib_strnlen(VitaProcess* p, VitaThread* t) {
    const char* s = host(p, arg(p, t, 0));
    uint32_t n = arg(p, t, 1), i = 0;
    while (i < n && s[i]) ++i;
    return i;
}
static uint32_t clib_strlcpy(VitaProcess* p, VitaThread* t) {
    char* d = host(p, arg(p, t, 0));
    const char* s = host(p, arg(p, t, 1));
    uint32_t n = arg(p, t, 2), len = (uint32_t)strlen(s);
    if (n) {
        uint32_t c = len < n - 1 ? len : n - 1;
        memmove(d, s, c);
        d[c] = 0;
    }
    return len;
}
static uint32_t clib_strlcat(VitaProcess* p, VitaThread* t) {
    char* d = host(p, arg(p, t, 0));
    const char* s = host(p, arg(p, t, 1));
    uint32_t n = arg(p, t, 2), dl = 0, len = (uint32_t)strlen(s);
    while (dl < n && d[dl]) ++dl;
    if (dl == n) return n + len;
    uint32_t c = len < n - dl - 1 ? len : n - dl - 1;
    memmove(d + dl, s, c);
    d[dl + c] = 0;
    return dl + len;
}
static uint32_t clib_tolower(VitaProcess* p, VitaThread* t) {
    uint32_t c = arg(p, t, 0);
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
static uint32_t clib_toupper(VitaProcess* p, VitaThread* t) {
    uint32_t c = arg(p, t, 0);
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}
static uint32_t clib_mspace_free(VitaProcess* p, VitaThread* t) {
    vita_heap_free(p, arg(p, t, 0), arg(p, t, 1));
    return 0;
}
static uint32_t zero(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
static uint32_t first_argument(VitaProcess* p, VitaThread* t) { return arg(p, t, 0); }

/* ---- memory and strings, in place ---- */
static uint32_t lc_memcpy(VitaProcess* p, VitaThread* t) {
    uint32_t d = arg(p, t, 0), n = arg(p, t, 2);
    if (n) memmove(host(p, d), host(p, arg(p, t, 1)), n);
    return d;
}
static uint32_t lc_memset(VitaProcess* p, VitaThread* t) {
    uint32_t d = arg(p, t, 0), n = arg(p, t, 2);
    if (n) memset(host(p, d), (int)arg(p, t, 1), n);
    return d;
}
static uint32_t lc_memcmp(VitaProcess* p, VitaThread* t) {
    uint32_t n = arg(p, t, 2);
    return n ? (uint32_t)memcmp(host(p, arg(p, t, 0)), host(p, arg(p, t, 1)), n) : 0;
}
static uint32_t lc_strlen(VitaProcess* p, VitaThread* t) { return (uint32_t)strlen(host(p, arg(p, t, 0))); }
static uint32_t lc_strcmp(VitaProcess* p, VitaThread* t) {
    return (uint32_t)strcmp(host(p, arg(p, t, 0)), host(p, arg(p, t, 1)));
}
static uint32_t lc_strncmp(VitaProcess* p, VitaThread* t) {
    return (uint32_t)strncmp(host(p, arg(p, t, 0)), host(p, arg(p, t, 1)), arg(p, t, 2));
}
static int casecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}
static uint32_t lc_strcasecmp(VitaProcess* p, VitaThread* t) {
    return (uint32_t)casecmp(host(p, arg(p, t, 0)), host(p, arg(p, t, 1)), SIZE_MAX);
}
static uint32_t lc_strncasecmp(VitaProcess* p, VitaThread* t) {
    return (uint32_t)casecmp(host(p, arg(p, t, 0)), host(p, arg(p, t, 1)), arg(p, t, 2));
}
static uint32_t lc_strcpy(VitaProcess* p, VitaThread* t) {
    uint32_t d = arg(p, t, 0);
    const char* s = host(p, arg(p, t, 1));
    memmove(host(p, d), s, strlen(s) + 1);
    return d;
}
static uint32_t lc_strncpy(VitaProcess* p, VitaThread* t) {
    uint32_t d = arg(p, t, 0), n = arg(p, t, 2);
    if (n) strncpy(host(p, d), host(p, arg(p, t, 1)), n);
    return d;
}
static uint32_t lc_strcat(VitaProcess* p, VitaThread* t) {
    uint32_t d = arg(p, t, 0);
    const char* s = host(p, arg(p, t, 1));
    char* end = host(p, d) + strlen(host(p, d));
    memmove(end, s, strlen(s) + 1);
    return d;
}
static uint32_t lc_strchr(VitaProcess* p, VitaThread* t) {
    return guest(p, strchr(host(p, arg(p, t, 0)), (int)(arg(p, t, 1) & 0xff)));
}
static uint32_t lc_memchr(VitaProcess* p, VitaThread* t) {
    uint32_t n = arg(p, t, 2);
    return n ? guest(p, memchr(host(p, arg(p, t, 0)), (int)(arg(p, t, 1) & 0xff), n)) : 0;
}
static uint32_t lc_strrchr(VitaProcess* p, VitaThread* t) {
    return guest(p, strrchr(host(p, arg(p, t, 0)), (int)(arg(p, t, 1) & 0xff)));
}
static uint32_t lc_strstr(VitaProcess* p, VitaThread* t) {
    return guest(p, strstr(host(p, arg(p, t, 0)), host(p, arg(p, t, 1))));
}
static uint32_t lc_strspn(VitaProcess* p, VitaThread* t) {
    return (uint32_t)strspn(host(p, arg(p, t, 0)), host(p, arg(p, t, 1)));
}
static uint32_t lc_strcspn(VitaProcess* p, VitaThread* t) {
    return (uint32_t)strcspn(host(p, arg(p, t, 0)), host(p, arg(p, t, 1)));
}
/* the wide strings: 16-bit wchar_t */
static uint32_t wide_length(VitaProcess* p, uint32_t s) {
    uint32_t n = 0;
    while (vita_read16(p, s + n * 2)) ++n;
    return n;
}
static uint32_t lc_wcslen(VitaProcess* p, VitaThread* t) { return wide_length(p, arg(p, t, 0)); }
static int wide_compare(VitaProcess* p, uint32_t a, uint32_t b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t x = vita_read16(p, a + i * 2), y = vita_read16(p, b + i * 2);
        if (x != y || !x) return x < y ? -1 : x > y;
    }
    return 0;
}
static uint32_t lc_wcscmp(VitaProcess* p, VitaThread* t) {
    return (uint32_t)wide_compare(p, arg(p, t, 0), arg(p, t, 1), UINT32_MAX);
}
static uint32_t lc_wcsncmp(VitaProcess* p, VitaThread* t) {
    return (uint32_t)wide_compare(p, arg(p, t, 0), arg(p, t, 1), arg(p, t, 2));
}
static uint32_t lc_wcschr(VitaProcess* p, VitaThread* t) {
    uint32_t s = arg(p, t, 0), c = arg(p, t, 1) & 0xffff;
    for (;; s += 2) {
        uint32_t u = vita_read16(p, s);
        if (u == c) return s;
        if (!u) return 0;
    }
}
static uint32_t lc_wmemchr(VitaProcess* p, VitaThread* t) {
    uint32_t s = arg(p, t, 0), c = arg(p, t, 1) & 0xffff, n = arg(p, t, 2);
    for (uint32_t i = 0; i < n; ++i)
        if (vita_read16(p, s + i * 2) == c) return s + i * 2;
    return 0;
}
static uint32_t lc_wmemcpy(VitaProcess* p, VitaThread* t) {
    uint32_t d = arg(p, t, 0), n = arg(p, t, 2);
    if (n) memmove(host(p, d), host(p, arg(p, t, 1)), (size_t)n * 2);
    return d;
}
static uint32_t lc_wmemset(VitaProcess* p, VitaThread* t) {
    uint32_t d = arg(p, t, 0), n = arg(p, t, 2);
    uint16_t c = (uint16_t)arg(p, t, 1);
    for (uint32_t i = 0; i < n; ++i) memcpy(host(p, d + i * 2), &c, 2);
    return d;
}
static uint32_t lc_wcsstr(VitaProcess* p, VitaThread* t) {
    uint32_t s = arg(p, t, 0), w = arg(p, t, 1), n = wide_length(p, w);
    for (;; s += 2) {
        if (!wide_compare(p, s, w, n)) return s;
        if (!vita_read16(p, s)) return 0;
    }
}
/* _Towctrans(c, which): wctrans's "tolower" (1) or "toupper" (2) */
static uint32_t lc_towctrans(VitaProcess* p, VitaThread* t) {
    uint32_t c = arg(p, t, 0), which = arg(p, t, 1);
    if (c >= 0x80) return c;
    return which == 1 ? (uint32_t)tolower((int)c) : which == 2 ? (uint32_t)toupper((int)c) : c;
}

/* ---- numbers from strings: the host's, on the guest's string ---- */
static uint32_t lc_strtol(VitaProcess* p, VitaThread* t) {
    char *s = host(p, arg(p, t, 0)), *end = s;
    long long v = strtoll(s, &end, (int)arg(p, t, 2));
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    if (arg(p, t, 1)) vita_write32(p, arg(p, t, 1), guest(p, end));
    return (uint32_t)(int32_t)v;
}
static uint32_t lc_stoul(VitaProcess* p, VitaThread* t) {
    char *s = host(p, arg(p, t, 0)), *end = s;
    unsigned long long v = strtoull(s, &end, (int)arg(p, t, 2));
    if (arg(p, t, 1)) vita_write32(p, arg(p, t, 1), guest(p, end));
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}
static uint32_t lc_stoull(VitaProcess* p, VitaThread* t) {
    char *s = host(p, arg(p, t, 0)), *end = s;
    unsigned long long v = strtoull(s, &end, (int)arg(p, t, 2));
    if (arg(p, t, 1)) vita_write32(p, arg(p, t, 1), guest(p, end));
    return vita_return64(t, v);
}
/* _Stod(s, endptr, pten): the number times 10 to pten */
static uint32_t lc_stod(VitaProcess* p, VitaThread* t) {
    char *s = host(p, arg(p, t, 0)), *end = s;
    double v = strtod(s, &end);
    int32_t pten = (int32_t)arg(p, t, 2);
    if (pten) v *= pow(10.0, pten);
    if (arg(p, t, 1)) vita_write32(p, arg(p, t, 1), guest(p, end));
    return vita_return_double(t, v);
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
        int width = -1, precision = -1;
        if (c == '*') {
            width = (int32_t)next_word(a);
            if (width < 0) left = true, width = -width, spec[n++] = '-';
            c = unit_at(p, fmt, wide, ++i);
        } else if (c >= '0' && c <= '9') {
            for (width = 0; c >= '0' && c <= '9'; c = unit_at(p, fmt, wide, ++i)) width = width * 10 + (int)(c - '0');
        }
        if (c == '.') {
            c = unit_at(p, fmt, wide, ++i);
            precision = 0;
            if (c == '*') {
                precision = (int32_t)next_word(a);
                c = unit_at(p, fmt, wide, ++i);
            } else {
                for (; c >= '0' && c <= '9'; c = unit_at(p, fmt, wide, ++i)) precision = precision * 10 + (int)(c - '0');
            }
        }
        if (width > 4096) width = 4096;
        if (precision > 4096) precision = 4096;
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
static Out print(VitaProcess* p, VitaThread* t, unsigned fmt, bool list, bool wide) {
    Args a = {p, t, fmt + 1, list ? arg(p, t, fmt + 1) : 0};
    Out o = {0};
    format(&a, arg(p, t, fmt), wide, &o);
    return o;
}
static uint32_t lc_printf(VitaProcess* p, VitaThread* t) {
    Out o = print(p, t, 0, false, false);
    to_host(stdout, &o);
    free(o.units);
    return (uint32_t)o.length;
}
static uint32_t lc_sprintf(VitaProcess* p, VitaThread* t) {
    Out o = print(p, t, 1, false, false);
    uint32_t r = to_buffer(p, arg(p, t, 0), UINT64_MAX, &o);
    free(o.units);
    return r;
}
static uint32_t lc_vsprintf(VitaProcess* p, VitaThread* t) {
    Out o = print(p, t, 1, true, false);
    uint32_t r = to_buffer(p, arg(p, t, 0), UINT64_MAX, &o);
    free(o.units);
    return r;
}
static uint32_t lc_snprintf(VitaProcess* p, VitaThread* t) {
    Out o = print(p, t, 2, false, false);
    uint32_t r = to_buffer(p, arg(p, t, 0), arg(p, t, 1), &o);
    free(o.units);
    return r;
}
static uint32_t lc_vsnprintf(VitaProcess* p, VitaThread* t) {
    Out o = print(p, t, 2, true, false);
    uint32_t r = to_buffer(p, arg(p, t, 0), arg(p, t, 1), &o);
    free(o.units);
    return r;
}
static uint32_t lc_swprintf(VitaProcess* p, VitaThread* t) {
    Out o = print(p, t, 2, false, true);
    uint32_t r = to_wide(p, arg(p, t, 0), arg(p, t, 1), &o);
    free(o.units);
    return r;
}
static uint32_t lc_vswprintf(VitaProcess* p, VitaThread* t) {
    Out o = print(p, t, 2, true, true);
    uint32_t r = to_wide(p, arg(p, t, 0), arg(p, t, 1), &o);
    free(o.units);
    return r;
}

/* ---- sscanf ---- */
static uint32_t lc_sscanf(VitaProcess* p, VitaThread* t) {
    const char* in = host(p, arg(p, t, 0));
    uint32_t fmt = arg(p, t, 1), assigned = 0;
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
        for (; c >= '0' && c <= '9'; c = p->mem[(uint32_t)(fmt + i++)]) width = width * 10 + (c - '0');
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
static uint32_t lc_fopen(VitaProcess* p, VitaThread* t) {
    char mode[8];
    vita_string(p, arg(p, t, 1), mode, sizeof mode);
    uint32_t flags = mode[0] == 'r' ? 0x0001u : mode[0] == 'w' ? 0x0002u | 0x0200u | 0x0400u
                     : mode[0] == 'a' ? 0x0002u | 0x0200u | 0x0100u : 0;
    if (strchr(mode, '+')) flags |= 0x0003u;
    if (!flags) return set_errno(p, t, EINVAL), 0;
    return open_stream(p, t, arg(p, t, 0), flags);
}
static uint32_t open_stream(VitaProcess* p, VitaThread* t, uint32_t path, uint32_t flags) {
    uint32_t fd = vita_io_open(p, path, flags);
    if (fd & 0x80000000u) return set_errno(p, t, fd & 0xffff), 0;
    uint32_t file = vita_heap_alloc(p, heap(p), 16, 8);
    if (!file) {
        vita_io_close(p, fd);
        return set_errno(p, t, ENOMEM), 0;
    }
    vita_write32(p, file, FILE_MAGIC), vita_write32(p, file + 4, fd);
    return file;
}
static uint32_t lc_fclose(VitaProcess* p, VitaThread* t) {
    uint32_t file = arg(p, t, 0), fd = file_fd(p, file);
    if (!fd || fd == STDOUT_FD || fd == STDERR_FD) return fd ? 0 : UINT32_MAX;
    vita_io_close(p, fd);
    vita_write32(p, file, 0);
    vita_heap_free(p, heap(p), file);
    return 0;
}
static uint32_t lc_fread(VitaProcess* p, VitaThread* t) {
    FILE* f = stream(p, arg(p, t, 3));
    uint32_t size = arg(p, t, 1), count = arg(p, t, 2), buffer = arg(p, t, 0);
    if (!f || !size || !count || !vita_mapped(p, buffer, (uint32_t)((uint64_t)size * count))) return 0;
    vita_watch_release(buffer, (uint32_t)((uint64_t)size * count)); /* the system writes it (watch.h) */
    return (uint32_t)fread(host(p, buffer), size, count, f);
}
static uint32_t lc_fwrite(VitaProcess* p, VitaThread* t) {
    FILE* f = stream(p, arg(p, t, 3));
    uint32_t size = arg(p, t, 1), count = arg(p, t, 2), buffer = arg(p, t, 0);
    if (!f || !size || !count || !vita_mapped(p, buffer, (uint32_t)((uint64_t)size * count))) return 0;
    return (uint32_t)fwrite(host(p, buffer), size, count, f);
}
static uint32_t lc_fseek(VitaProcess* p, VitaThread* t) {
    FILE* f = stream(p, arg(p, t, 0));
    return f && !fseek(f, (long)(int32_t)arg(p, t, 1), (int)arg(p, t, 2)) ? 0 : UINT32_MAX;
}
static uint32_t lc_ftell(VitaProcess* p, VitaThread* t) {
    FILE* f = stream(p, arg(p, t, 0));
    return f ? (uint32_t)ftell(f) : UINT32_MAX;
}
static uint32_t lc_fflush(VitaProcess* p, VitaThread* t) {
    FILE* f = arg(p, t, 0) ? stream(p, arg(p, t, 0)) : stdout;
    if (f) fflush(f);
    return 0;
}
static uint32_t lc_fputs(VitaProcess* p, VitaThread* t) {
    FILE* f = stream(p, arg(p, t, 1));
    if (!f) return UINT32_MAX;
    fputs(host(p, arg(p, t, 0)), f);
    if (f == stdout || f == stderr) fflush(f);
    return 0;
}
static uint32_t lc_fprintf(VitaProcess* p, VitaThread* t) {
    FILE* f = stream(p, arg(p, t, 0));
    Out o = print(p, t, 1, false, false);
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
static uint32_t lc_exit(VitaProcess* p, VitaThread* t) {
    p->exited = true;
    p->exit_code = (int32_t)arg(p, t, 0);
    return 0;
}
static uint32_t lc_abort(VitaProcess* p, VitaThread* t) { return fatal(p, t, "abort()"); }
/* setjmp(env): r4 to r11, sp and lr, then d8 to d15, as only longjmp reads them */
static uint32_t lc_setjmp(VitaProcess* p, VitaThread* t) {
    uint32_t env = arg(p, t, 0);
    for (unsigned i = 0; i < 10; ++i) vita_write32(p, env + i * 4, t->cpu.r[4 + i]);
    for (unsigned i = 0; i < 8; ++i) vita_write64(p, env + 40 + i * 8, t->cpu.d[8 + i]);
    return 0;
}
static uint32_t lc_longjmp(VitaProcess* p, VitaThread* t) {
    uint32_t env = arg(p, t, 0), value = arg(p, t, 1);
    for (unsigned i = 0; i < 10; ++i) t->cpu.r[4 + i] = vita_read32(p, env + i * 4);
    for (unsigned i = 0; i < 8; ++i)
        t->cpu.d[8 + i] = vita_read32(p, env + 40 + i * 8) | (uint64_t)vita_read32(p, env + 44 + i * 8) << 32;
    return value ? value : 1; /* returned from setjmp's call, by the stub's bx lr */
}
/* __tls_get_addr(&{module, offset}): the thread's copy of the module's thread-local data, made at its first use */
static uint32_t lc_tls_get_addr(VitaProcess* p, VitaThread* t) {
    const VitaImage* image = &p->image;
    if (!t->module_tls) {
        uint32_t block = image->tls_memsz ? vita_alloc(p, &p->user, image->tls_memsz, PAGE) : 0;
        if (!block) return fatal(p, t, "no thread-local data for a thread");
        memcpy(host(p, block), host(p, image->tls_image), image->tls_filesz);
        t->module_tls = block;
    }
    return t->module_tls + vita_read32(p, arg(p, t, 0) + 4);
}
static uint32_t lc_errno_loc(VitaProcess* p, VitaThread* t) {
    (void)p;
    return t->tls + VITA_TLS_ERRNO;
}
/* __cxa_guard_acquire(guard): 1 when the object is to be made (its guard's first byte clear), one thread at a time */
static uint32_t cxa_guard_acquire(VitaProcess* p, VitaThread* t) { return !(p->mem[arg(p, t, 0)] & 1); }
static uint32_t cxa_guard_release(VitaProcess* p, VitaThread* t) {
    p->mem[arg(p, t, 0)] = 1;
    return 0;
}
static uint32_t lc_rand(VitaProcess* p, VitaThread* t) {
    (void)t;
    p->libc.rand = p->libc.rand * 1103515245u + 12345u;
    return (p->libc.rand >> 16) & 0x7fff;
}
static uint32_t lc_srand(VitaProcess* p, VitaThread* t) {
    p->libc.rand = arg(p, t, 0);
    return 0;
}
/* ldiv(n, d): its ldiv_t returned in memory, the address in r0 */
static uint32_t lc_ldiv(VitaProcess* p, VitaThread* t) {
    uint32_t out = arg(p, t, 0);
    int32_t n = (int32_t)arg(p, t, 1), d = (int32_t)arg(p, t, 2);
    int32_t q = d ? (d == -1 ? (int32_t)(0u - (uint32_t)n) : n / d) : 0, r = d ? (int32_t)((uint32_t)n - (uint32_t)q * (uint32_t)d) : 0;
    vita_write32(p, out, (uint32_t)q), vita_write32(p, out + 4, (uint32_t)r);
    return out;
}
/* time(t): a 32-bit time_t, as SCE's libc has it */
static uint32_t lc_time(VitaProcess* p, VitaThread* t) {
    uint32_t now = (uint32_t)time(NULL);
    if (arg(p, t, 0)) vita_write32(p, arg(p, t, 0), now);
    return now;
}
static uint32_t lc_gmtime(VitaProcess* p, VitaThread* t) {
    uint32_t d = data(p), at = arg(p, t, 0);
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

/* ---- SceLibm: the hard-float ABI's, S0 and S1 or D0 and D1 in, S0 or D0 out ---- */
#define F1(name, expr)                                                                                                 \
    static uint32_t name(VitaProcess* p, VitaThread* t) {                                                              \
        (void)p;                                                                                                       \
        float x = vita_float(t, 0), y = vita_float(t, 1);                                                              \
        (void)y;                                                                                                       \
        return vita_return_float(t, expr);                                                                             \
    }
#define D1(name, expr)                                                                                                 \
    static uint32_t name(VitaProcess* p, VitaThread* t) {                                                              \
        (void)p;                                                                                                       \
        double x = vita_double(t, 0), y = vita_double(t, 1);                                                           \
        (void)y;                                                                                                       \
        return vita_return_double(t, expr);                                                                            \
    }
F1(lm_acosf, acosf(x))
F1(lm_asinf, asinf(x))
F1(lm_atanf, atanf(x))
F1(lm_atan2f, atan2f(x, y))
F1(lm_tanf, tanf(x))
F1(lm_expf, expf(x))
F1(lm_powf, powf(x, y))
F1(lm_fmodf, fmodf(x, y))
F1(lm_ceilf, ceilf(x))
F1(lm_floorf, floorf(x))
F1(lm_fabsf, fabsf(x))
F1(lm_sinf, sinf(x))
F1(lm_cosf, cosf(x))
F1(lm_sqrtf, sqrtf(x))
F1(lm_logf, logf(x))
D1(lm_pow, pow(x, y))
D1(lm_fmod, fmod(x, y))
D1(lm_exp, exp(x))
D1(lm_sqrt, sqrt(x))
D1(lm_floor, floor(x))
D1(lm_ceil, ceil(x))
static uint32_t lm_ldexpf(VitaProcess* p, VitaThread* t) {
    return vita_return_float(t, ldexpf(vita_float(t, 0), (int)(int32_t)arg(p, t, 0)));
}
/* modff(x, float* integer) */
static uint32_t lm_modff(VitaProcess* p, VitaThread* t) {
    float whole, part = modff(vita_float(t, 0), &whole);
    uint32_t bits;
    memcpy(&bits, &whole, sizeof bits);
    if (arg(p, t, 0)) vita_write32(p, arg(p, t, 0), bits);
    return vita_return_float(t, part);
}
/* Dinkumware's _FSin(x, quadrant offset): sin(x + offset * pi/2), cos for offset 1 */
static uint32_t lm_fsin(VitaProcess* p, VitaThread* t) {
    float x = vita_float(t, 0);
    switch (arg(p, t, 0) & 3) {
        case 0: return vita_return_float(t, sinf(x));
        case 1: return vita_return_float(t, cosf(x));
        case 2: return vita_return_float(t, -sinf(x));
        default: return vita_return_float(t, -cosf(x));
    }
}
/* _FLog(x, base): 0 the natural, 1 base 10, 2 base 2 */
static uint32_t lm_flog(VitaProcess* p, VitaThread* t) {
    float x = vita_float(t, 0);
    uint32_t base = arg(p, t, 0);
    return vita_return_float(t, base == 1 ? log10f(x) : base == 2 ? log2f(x) : logf(x));
}
/* _FDtest(float* x): its class, Dinkumware's codes */
static uint32_t lm_fdtest(VitaProcess* p, VitaThread* t) {
    uint32_t bits = vita_read32(p, arg(p, t, 0));
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
D1(lm_fabs, fabs(x))
D1(lm_sin, sin(x))
D1(lm_cos, cos(x))
D1(lm_tan, tan(x))
D1(lm_asin, asin(x))
D1(lm_acos, acos(x))
D1(lm_atan, atan(x))
D1(lm_atan2, atan2(x, y))
D1(lm_log, log(x))
D1(lm_log10, log10(x))
D1(lm_log2, log2(x))
D1(lm_exp2, exp2(x))
D1(lm_trunc, trunc(x))
D1(lm_round, round(x))
D1(lm_rint, rint(x))
D1(lm_nearbyint, nearbyint(x))
D1(lm_cbrt, cbrt(x))
D1(lm_sinh, sinh(x))
D1(lm_cosh, cosh(x))
D1(lm_tanh, tanh(x))
D1(lm_hypot, hypot(x, y))
D1(lm_copysign, copysign(x, y))
D1(lm_fmax, fmax(x, y))
D1(lm_fmin, fmin(x, y))
F1(lm_truncf, truncf(x))
F1(lm_roundf, roundf(x))
F1(lm_rintf, rintf(x))
F1(lm_nearbyintf, nearbyintf(x))
F1(lm_log10f, log10f(x))
F1(lm_log2f, log2f(x))
F1(lm_exp2f, exp2f(x))
F1(lm_cbrtf, cbrtf(x))
F1(lm_sinhf, sinhf(x))
F1(lm_coshf, coshf(x))
F1(lm_tanhf, tanhf(x))
F1(lm_hypotf, hypotf(x, y))
F1(lm_copysignf, copysignf(x, y))
F1(lm_fmaxf, fmaxf(x, y))
F1(lm_fminf, fminf(x, y))
/* ldexp(x, int exponent) and frexp(x, int* exponent): the double in D0, the int in R0 */
static uint32_t lm_ldexp(VitaProcess* p, VitaThread* t) {
    return vita_return_double(t, ldexp(vita_double(t, 0), (int)(int32_t)arg(p, t, 0)));
}
static uint32_t lm_frexp(VitaProcess* p, VitaThread* t) {
    int exponent = 0;
    double mantissa = frexp(vita_double(t, 0), &exponent);
    uint32_t out = arg(p, t, 0);
    if (out && vita_mapped(p, out, 4)) vita_write32(p, out, (uint32_t)exponent);
    return vita_return_double(t, mantissa);
}
/* modf(x, double* integer): the fraction in D0, the integer part through R0 */
static uint32_t lm_modf(VitaProcess* p, VitaThread* t) {
    double whole, part = modf(vita_double(t, 0), &whole);
    uint32_t out = arg(p, t, 0), words[2];
    memcpy(words, &whole, sizeof words);
    if (out && vita_mapped(p, out, 8)) vita_write32(p, out, words[0]), vita_write32(p, out + 4, words[1]);
    return vita_return_double(t, part);
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
static uint32_t lm_dtest(VitaProcess* p, VitaThread* t) {
    uint32_t at = arg(p, t, 0);
    if (!at || !vita_mapped(p, at, 8)) return 0;
    uint32_t words[2] = {vita_read32(p, at), vita_read32(p, at + 4)};
    double x;
    memcpy(&x, words, sizeof x);
    return (uint32_t)dinkum_class(x);
}
static uint32_t lm_dsign(VitaProcess* p, VitaThread* t) {
    (void)p;
    return signbit(vita_double(t, 0)) ? 0x8000u : 0;
}
static uint32_t lm_fdsign(VitaProcess* p, VitaThread* t) {
    (void)p;
    return signbit(vita_float(t, 0)) ? 0x8000u : 0;
}
/* _Stolx(const char* s, char** end, int base, int* error): strtol, the error set when the value is out of range */
static uint32_t lc_stolx(VitaProcess* p, VitaThread* t) {
    char *s = host(p, arg(p, t, 0)), *end = s;
    long long v = strtoll(s, &end, (int)arg(p, t, 2));
    uint32_t error = arg(p, t, 3);
    if (arg(p, t, 1)) vita_write32(p, arg(p, t, 1), guest(p, end));
    if (v > INT32_MAX || v < INT32_MIN) {
        if (error && vita_mapped(p, error, 4)) vita_write32(p, error, 1);
        return v > 0 ? (uint32_t)INT32_MAX : (uint32_t)INT32_MIN;
    }
    return (uint32_t)(int32_t)v;
}

/* _Fiopen(path, openmode, protection): what ifstream and ofstream open with. The openmode's bits: in 1, out 2, ate 4,
   app 8, trunc 0x10, binary 0x20 */
static uint32_t cxx_fiopen(VitaProcess* p, VitaThread* t) {
    uint32_t mode = arg(p, t, 1), flags = 0;
    if (mode & 1) flags |= 0x0001u;
    if (mode & 2) flags |= 0x0002u;
    if ((mode & 3) == 2 || (mode & 0x10)) flags |= 0x0200u | 0x0400u; /* out alone makes the file and empties it */
    if (mode & 8) flags = (flags & ~0x0400u) | 0x0002u | 0x0200u | 0x0100u;
    if (!flags) return set_errno(p, t, EINVAL), 0;
    return open_stream(p, t, arg(p, t, 0), flags);
}
/* wide strings are 16-bit on the Vita */
static uint32_t lc_wmemcmp(VitaProcess* p, VitaThread* t) {
    uint32_t a = arg(p, t, 0), b = arg(p, t, 1), n = arg(p, t, 2);
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t x, y;
        memcpy(&x, host(p, a + i * 2), 2), memcpy(&y, host(p, b + i * 2), 2);
        if (x != y) return x < y ? UINT32_MAX : 1;
    }
    return 0;
}
static uint32_t lc_labs(VitaProcess* p, VitaThread* t) {
    int32_t v = (int32_t)arg(p, t, 0);
    return v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
}
static uint32_t lc_imaxabs(VitaProcess* p, VitaThread* t) {
    int64_t v = (int64_t)((uint64_t)arg(p, t, 1) << 32 | arg(p, t, 0));
    return vita_return64(t, v < 0 ? 0u - (uint64_t)v : (uint64_t)v);
}
/* _Log(x, base) as _FLog's, in double */
static uint32_t lm_log_base(VitaProcess* p, VitaThread* t) {
    double x = vita_double(t, 0);
    uint32_t base = arg(p, t, 0);
    return vita_return_double(t, base == 1 ? log10(x) : base == 2 ? log2(x) : log(x));
}

/* ---- SceLibstdcxx: what the game's code throws ends it ---- */
static uint32_t cxx_length_error(VitaProcess* p, VitaThread* t) { return fatal(p, t, "std::length_error thrown"); }
static uint32_t cxx_out_of_range(VitaProcess* p, VitaThread* t) { return fatal(p, t, "std::out_of_range thrown"); }
static uint32_t cxx_raise(VitaProcess* p, VitaThread* t) { return fatal(p, t, "a C++ exception thrown"); }
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
static uint32_t cxx_typeinfo_name(VitaProcess* p, VitaThread* t) {
    static unsigned shown;
    uint32_t info = arg(p, t, 0);
    if (shown < 3 && info && vita_mapped(p, info, 16)) {
        ++shown;
        printf("vita: type_info::name(0x%08" PRIx32 ") words %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
               info, vita_read32(p, info), vita_read32(p, info + 4), vita_read32(p, info + 8),
               vita_read32(p, info + 12));
    }
    return typeinfo_name(p, info);
}
static uint32_t cxx_typeinfo_eq(VitaProcess* p, VitaThread* t) {
    return (uint32_t)typeinfo_same(p, arg(p, t, 0), arg(p, t, 1));
}
static uint32_t cxx_typeinfo_ne(VitaProcess* p, VitaThread* t) {
    return (uint32_t)!typeinfo_same(p, arg(p, t, 0), arg(p, t, 1));
}
static uint32_t cxa_allocate_exception(VitaProcess* p, VitaThread* t) {
    uint32_t r = vita_heap_alloc(p, heap(p), arg(p, t, 0) + 128, 8);
    return r ? r : fatal(p, t, "no memory for an exception");
}
/* ios_base::clear(state, reraise). The game's inline code reads the state at +8 and the rdbuf at +0x24, so the
   exception mask is taken as the word after the state. The first calls are logged to check it */
static uint32_t ios_clear(VitaProcess* p, VitaThread* t) {
    static unsigned shown;
    uint32_t self = arg(p, t, 0), state = arg(p, t, 1);
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
static uint32_t ios_init(VitaProcess* p, VitaThread* t) {
    uint32_t self = arg(p, t, 0);
    vita_write32(p, self + 8, 0);
    vita_write32(p, self + 12, 0);
    vita_write32(p, self + 16, 0x201);
    vita_write32(p, self + 20, 6);
    vita_write32(p, self + 24, 0);
    return self;
}
/* std::exception::exception(this): its vtable one whose functions do nothing */
static uint32_t cxx_exception(VitaProcess* p, VitaThread* t) {
    uint32_t self = arg(p, t, 0), d = data(p);
    if (d) vita_write32(p, self, d + EMPTY_VTABLE + 8);
    return self;
}

/* ---- the Clib's printf ---- */
static uint32_t clib_snprintf(VitaProcess* p, VitaThread* t) { return lc_snprintf(p, t); }
static uint32_t clib_vsnprintf(VitaProcess* p, VitaThread* t) { return lc_vsnprintf(p, t); }

const VitaHle vita_hle_libc[] = {
    /* SceLibc */
    {0x775a0cb2u, "malloc", lc_malloc},
    {0x5b9bb802u, "free", lc_free},
    {0x006b54bau, "realloc", lc_realloc},
    {0xe7ec3d0bu, "calloc", lc_calloc},
    {0xa9363e6bu, "memalign", lc_memalign},
    {0x608ac135u, "reallocalign", lc_reallocalign},
    {0x54a54eb1u, "malloc_usable_size", lc_malloc_usable_size},
    {0x7205bfdbu, "memcpy", lc_memcpy},
    {0xaf5c218du, "memmove", lc_memcpy},
    {0x6dc1f0d8u, "memset", lc_memset},
    {0x7747f6d7u, "memcmp", lc_memcmp},
    {0x2f3e5b16u, "memchr", lc_memchr},
    {0x8aecc873u, "strlen", lc_strlen},
    {0x1b58fa3bu, "strcmp", lc_strcmp},
    {0xe4299dcbu, "strncmp", lc_strncmp},
    {0x184c4b07u, "strcasecmp", lc_strcasecmp},
    {0xaf1ca2f1u, "strncasecmp", lc_strncasecmp},
    {0x85b924b7u, "strcpy", lc_strcpy},
    {0x9f87712du, "strncpy", lc_strncpy},
    {0x1434fa46u, "strcat", lc_strcat},
    {0xb9336e16u, "strchr", lc_strchr},
    {0xcefdd143u, "strrchr", lc_strrchr},
    {0x0d5200cbu, "strstr", lc_strstr},
    {0x4203b663u, "strspn", lc_strspn},
    {0x0e29d27au, "strcspn", lc_strcspn},
    {0x7a08be70u, "wmemchr", lc_wmemchr},
    {0xd9f9ddcdu, "wmemcpy", lc_wmemcpy},
    {0x4d04a480u, "wmemset", lc_wmemset},
    {0xa778a14bu, "wcslen", lc_wcslen},
    {0xf42128b9u, "wcscmp", lc_wcscmp},
    {0xaaa6aaa2u, "wcsncmp", lc_wcsncmp},
    {0xc1587971u, "wcschr", lc_wcschr},
    {0x5be328eeu, "wcsstr", lc_wcsstr},
    {0x9d885076u, "_Towctrans", lc_towctrans},
    {0x181827edu, "strtol", lc_strtol},
    {0xe71c5cdeu, "_Stoul", lc_stoul},
    {0x6794b3c6u, "_Stoull", lc_stoull},
    {0xbf94193bu, "_Stod", lc_stod},
    {0x9a004680u, "printf", lc_printf},
    {0x7449b359u, "sprintf", lc_sprintf},
    {0x802fddf9u, "vsprintf", lc_vsprintf},
    {0xa1bff606u, "snprintf", lc_snprintf},
    {0xfe83f2e4u, "vsnprintf", lc_vsnprintf},
    {0x1b581bebu, "swprintf", lc_swprintf},
    {0x572dab57u, "vswprintf", lc_vswprintf},
    {0xec585241u, "sscanf", lc_sscanf},
    {0xfffbe239u, "fopen", lc_fopen},
    {0xec97321cu, "fclose", lc_fclose},
    {0xb31c73a9u, "fread", lc_fread},
    {0x8bcdcc4eu, "fwrite", lc_fwrite},
    {0xc3a7cde1u, "fseek", lc_fseek},
    {0x41c2af95u, "ftell", lc_ftell},
    {0x5aad2996u, "fflush", lc_fflush},
    {0xc8ff13e5u, "fputs", lc_fputs},
    {0xe0c79764u, "fprintf", lc_fprintf},
    {0x826bbbafu, "exit", lc_exit},
    {0x20fe0fffu, "abort", lc_abort},
    {0x50b326ceu, "setjmp", lc_setjmp},
    {0x2d81c8c8u, "longjmp", lc_longjmp},
    {0xbc529b7bu, "__tls_get_addr", lc_tls_get_addr},
    {0x54f87eacu, "_sceLdTlsRegisterModuleInfo", zero},
    {0x9e248b76u, "_sceLibcErrnoLoc", lc_errno_loc},
    {0xbfe02b3au, "__cxa_set_dso_handle_main", zero},
    {0x33b83b70u, "__cxa_atexit", zero},
    {0xedc939e1u, "__aeabi_atexit", zero},
    {0xd0310e31u, "__cxa_guard_acquire", cxa_guard_acquire},
    {0x4ed1056fu, "__cxa_guard_release", cxa_guard_release},
    {0xc0883865u, "rand", lc_rand},
    {0x3aad41b0u, "srand", lc_srand},
    {0xd63330dau, "ldiv", lc_ldiv},
    {0xdae8d60fu, "time", lc_time},
    {0xf283cfe3u, "gmtime", lc_gmtime},
    /* SceLibm */
    {0x27eab8c1u, "acosf", lm_acosf},
    {0x3a3e5424u, "asinf", lm_asinf},
    {0xd78fc94eu, "atanf", lm_atanf},
    {0x4e09dd53u, "atan2f", lm_atan2f},
    {0xa98e941bu, "tanf", lm_tanf},
    {0x56473bc7u, "expf", lm_expf},
    {0x6dea815au, "powf", lm_powf},
    {0x1cd8f88eu, "fmodf", lm_fmodf},
    {0x6bbfec89u, "ceilf", lm_ceilf},
    {0xcd7c05bdu, "floorf", lm_floorf},
    {0x75348906u, "fabsf", lm_fabsf},
    {0x0e61e016u, "ldexpf", lm_ldexpf},
    {0x5d7a7eb2u, "modff", lm_modff},
    {0x640db443u, "pow", lm_pow},
    {0x798587e4u, "fmod", lm_fmod},
    {0xeb027358u, "exp", lm_exp},
    {0x7f00b590u, "sinf", lm_sinf},
    {0x127f8302u, "cosf", lm_cosf},
    {0xba3f6937u, "sqrtf", lm_sqrtf},
    {0x811ed68bu, "logf", lm_logf},
    {0xda227fccu, "sqrt", lm_sqrt},
    {0x22bb8237u, "floor", lm_floor},
    {0x63f05bd6u, "ceil", lm_ceil},
    {0x4a496bc0u, "_FSin", lm_fsin},
    {0xd5bd8d5cu, "_FLog", lm_flog},
    {0xc5b9c8d8u, "_FDtest", lm_fdtest},
    {0x1167b5d2u, "modf", lm_modf},
    {0x27a55170u, "_Dtest", lm_dtest},
    {0x5bd0f71cu, "_Dsign", lm_dsign},
    {0xc4f7e42cu, "_FDsign", lm_fdsign},
    {0x52ddcdafu, "_Stolx", lc_stolx},
    {0x3e672be3u, "fabs", lm_fabs},
    {0xb5519ff0u, "sin", lm_sin},
    {0x061d0244u, "cos", lm_cos},
    {0x5bae40b0u, "tan", lm_tan},
    {0x4016b2e6u, "asin", lm_asin},
    {0xd72b5acbu, "acos", lm_acos},
    {0x516d9970u, "atan", lm_atan},
    {0xc9be3f05u, "atan2", lm_atan2},
    {0x6037c48fu, "log", lm_log},
    {0xcf65f098u, "log10", lm_log10},
    {0x73afee5fu, "log2", lm_log2},
    {0x9b18f38fu, "exp2", lm_exp2},
    {0x0212323eu, "trunc", lm_trunc},
    {0x64d37996u, "round", lm_round},
    {0x943f218fu, "rint", lm_rint},
    {0x877187c4u, "nearbyint", lm_nearbyint},
    {0xacc0dc5au, "cbrt", lm_cbrt},
    {0xf2c0af49u, "sinh", lm_sinh},
    {0x110195e7u, "cosh", lm_cosh},
    {0x26cd78cau, "tanh", lm_tanh},
    {0x2d2cd795u, "hypot", lm_hypot},
    {0x0b918d13u, "copysign", lm_copysign},
    {0xbe30cc1eu, "fmax", lm_fmax},
    {0x2abbdff7u, "fmin", lm_fmin},
    {0x0056061bu, "ldexp", lm_ldexp},
    {0x59197427u, "frexp", lm_frexp},
    {0x090b899fu, "truncf", lm_truncf},
    {0xaaf31896u, "roundf", lm_roundf},
    {0xcace5a19u, "rintf", lm_rintf},
    {0xd56e78f6u, "nearbyintf", lm_nearbyintf},
    {0xfd2a3464u, "log10f", lm_log10f},
    {0x4095dbdbu, "log2f", lm_log2f},
    {0x79415bd3u, "exp2f", lm_exp2f},
    {0xd1699f4du, "cbrtf", lm_cbrtf},
    {0xb5838e7du, "sinhf", lm_sinhf},
    {0x61de0770u, "coshf", lm_coshf},
    {0xc4847578u, "tanhf", lm_tanhf},
    {0xa397b929u, "hypotf", lm_hypotf},
    {0x16eb9e63u, "copysignf", lm_copysignf},
    {0x7004fa75u, "fmaxf", lm_fmaxf},
    {0x7673cc1eu, "fminf", lm_fminf},
    /* SceLibstdcxx */
    {0xf99ed5acu, "_Znwj", cxx_new},
    {0xe7fb2bf4u, "_Znaj", cxx_new},
    {0x0ae71dc3u, "_ZnwjRKSt9nothrow_t", cxx_new_nothrow},
    {0x31c62481u, "_ZnajRKSt9nothrow_t", cxx_new_nothrow},
    {0x72293931u, "_ZdlPv", cxx_delete},
    {0x91b0dc47u, "_ZdaPv", cxx_delete},
    {0x87ef85ffu, "_ZdlPvRKSt9nothrow_t", cxx_delete},
    {0xa7241f09u, "_ZdaPvRKSt9nothrow_t", cxx_delete},
    {0x1eb89099u, "_ZdlPvS_", cxx_delete_sized},
    {0x3688ffdau, "_ZdaPvS_", cxx_delete_sized},
    {0xf9fab558u, "_ZNSt6_Mutex5_LockEv", zero},
    {0x0402c9f8u, "_ZNSt6_Mutex7_UnlockEv", zero},
    {0xa4f99ae7u, "_ZNSt6_MutexC1Ev", first_argument},
    {0x011ceb00u, "_ZSt18uncaught_exceptionv", zero},
    {0x78cb190eu, "_ZNSt8ios_base5_InitEv", ios_init},
    {0x9b37cad9u, "_ZNSt6_MutexD1Ev", zero},
    {0xdce89e71u, "_ZNSt8ios_baseD2Ev", zero},
    {0x40ea90d5u, "_ZNSt8ios_baseD1Ev", zero},
    {0x5e60b2b3u, "_ZNSt8ios_base4InitC1Ev", first_argument},
    {0xabb11cf9u, "_ZNSt6_WinitC1Ev", first_argument},
    {0xc9de8208u, "_ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb", ios_clear},
    {0xdb15f0fcu, "_ZNKSt9type_infoeqERKS_", cxx_typeinfo_eq},
    {0x9994f9feu, "_ZNKSt9type_infoneERKS_", cxx_typeinfo_ne},
    {0x8c93efdau, "__cxa_allocate_exception", cxa_allocate_exception},
    {0xbae38df9u, "_ZNKSt9type_info4nameEv", cxx_typeinfo_name},
    {0xfd276300u, "_ZSt7_FiopenPKcNSt5_IosbIiE9_OpenmodeEi", cxx_fiopen},
    {0x9da92617u, "_ZNSt6_MutexC1ESt14_Uninitialized", first_argument},
    {0x9864c99fu, "wmemcmp", lc_wmemcmp},
    {0x53f7eb4bu, "wmemmove", lc_wmemcpy},
    {0xbcea304bu, "labs", lc_labs},
    {0x1118b49fu, "imaxabs", lc_imaxabs},
    {0x5044fc32u, "_Lockfilelock", zero},
    {0xfd5dd98cu, "_Unlockfilelock", zero},
    {0x67e99979u, "_Log", lm_log_base},
    {0xf87e6098u, "__cxa_throw", cxx_raise},
    {0x596ba786u, "_ZNSt8bad_castC2Ev", first_argument},
    {0xe8d59ab5u, "_ZNKSt12_String_base5_XlenEv", cxx_length_error},
    {0x473dc636u, "_ZNKSt12_String_base5_XranEv", cxx_out_of_range},
    {0x0656be32u, "_ZNKSt9exception6_RaiseEv", cxx_raise},
    {0x00e08376u, "_ZNSt9exceptionC2Ev", cxx_exception},
    {0x5fae79bfu, "_ZNSt12out_of_rangeD1Ev", first_argument},
    {0xa873d7f9u, "_ZNSt12length_errorD1Ev", first_argument},
    /* SceLibKernel's Clib */
    {0x14e9dbd7u, "sceClibMemcpy", lc_memcpy},
    {0x632980d7u, "sceClibMemset", lc_memset},
    {0x9cc2bfdfu, "sceClibMemcmp", lc_memcmp},
    {0x6e728aaeu, "sceClibStrrchr", lc_strrchr},
    {0xb54c0be4u, "sceClibStrncasecmp", lc_strncasecmp},
    {0xfa26bc62u, "sceClibPrintf", lc_printf},
    {0x8cba03d5u, "sceClibSnprintf", clib_snprintf},
    {0xfa6be467u, "sceClibVsnprintf", clib_vsnprintf},
    {0x3b9e301au, "sceClibMspaceCreate", clib_mspace_create},
    {0x86ef7680u, "sceClibMspaceMalloc", clib_mspace_malloc},
    {0x9c56b4d1u, "sceClibMspaceFree", clib_mspace_free},
    {0xae1a21ecu, "sceClibMspaceDestroy", zero},
    {0x736753c8u, "sceClibMemmove", lc_memcpy},
    {0xa18068fcu, "sceClibMemmoveChk", lc_memcpy},
    {0x1f94ef45u, "sceClibMemcpyChk", lc_memcpy},
    {0x2e3b02a1u, "sceClibMemcpy_safe", lc_memcpy},
    {0xd19b6dc5u, "sceClibMemsetChk", lc_memset},
    {0x407d6153u, "sceClibMemchr", lc_memchr},
    {0x614076b7u, "sceClibStrchr", lc_strchr},
    {0xa2fb4d9du, "sceClibStrcmp", lc_strcmp},
    {0x660d1f6du, "sceClibStrncmp", lc_strncmp},
    {0xc458d60au, "sceClibStrncpy", lc_strncpy},
    {0xe265498bu, "sceClibStrstr", lc_strstr},
    {0xac595e68u, "sceClibStrnlen", clib_strnlen},
    {0x2cdfcd1cu, "sceClibStrlcpy", clib_strlcpy},
    {0x70cbc2d5u, "sceClibStrlcat", clib_strlcat},
    {0x4c5471bcu, "sceClibTolower", clib_tolower},
    {0xb0335388u, "sceClibToupper", clib_toupper},
    {0x678374adu, "sceClibMspaceCalloc", clib_mspace_calloc},
    {0x3c847d57u, "sceClibMspaceMemalign", clib_mspace_memalign},
    {0x774891d6u, "sceClibMspaceRealloc", clib_mspace_realloc},
    {0x4373b548u, "__sce_aeabi_idiv0", zero},
    {0xfb235848u, "__sce_aeabi_ldiv0", zero},
    {0, NULL, NULL},
};
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