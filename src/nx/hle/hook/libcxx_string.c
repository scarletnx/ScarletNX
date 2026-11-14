/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* std::string (libc++'s basic_string<char, char_traits<char>, allocator<char>>) on guest memory: the string family of
   hooks.hook. The object is libc++'s layout, the same in every libc++ a game was built with (the old __cap_ | 1 and
   the newer __is_long_ bit field store the same bytes):

     short: byte 0 the size << 1 (bit 0 clear), the characters from byte 1, at most 22 and a NUL
     long:  word 0 the allocation's size (the capacity + 1, even) | 1, word 1 the size, word 2 the characters

   so the game's inlined code (size(), data(), operator[]) reads what the handlers write. A buffer comes from the
   guest's operator new and goes back to its operator delete: the host's arena (alloc.c) when the alloc family took
   them, else a call of the guest's own. What would throw (a position past the end, a length past max_size) runs the
   function's own code, before the handler changes anything, so the game's exception is the one it would get.
   A 32-bit game's strings are another layout, so every hook is for a 64-bit game */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/kernel/kernel.h"
#include <stdlib.h>
#include <string.h>

#define SHORT_CAP 22u
#define NPOS      (~0ull)
#define MAX_SIZE  (1ull << 62) /* past it, the function's own code throws length_error */

#define H(a) ((char*)hook_host(c, a))
#define OWN()                                                                                                          \
    do {                                                                                                               \
        hook_run_original(c, c->pc);                                                                                   \
        return c->x[0];                                                                                                \
    } while (0)
#define OWN_VOID()                                                                                                     \
    do {                                                                                                               \
        hook_run_original(c, c->pc);                                                                                   \
        return;                                                                                                        \
    } while (0)

typedef struct {
    u64 self, size, cap, data; /* data: the characters' guest address */
    int is_long;
} Str;

static Str get(CPU* c, u64 self) {
    const u8* r = hook_host(c, self);
    Str s;
    s.self = self;
    s.is_long = r[0] & 1;
    if (s.is_long) {
        s.cap = (snx_ld_le64(r) & ~1ull) - 1;
        s.size = snx_ld_le64(r + 8);
        s.data = snx_ld_le64(r + 16);
    } else {
        s.cap = SHORT_CAP;
        s.size = r[0] >> 1;
        s.data = self + 1;
    }
    return s;
}
static void set_size(CPU* c, Str* s, u64 n) {
    if (s->is_long)
        snx_st_le64(hook_host(c, s->self + 8), n);
    else
        *hook_host(c, s->self) = (u8)(n << 1);
    H(s->data)[n] = 0;
    s->size = n;
}
static void set_long(CPU* c, Str* s, u64 data, u64 cap, u64 size) {
    u8* r = hook_host(c, s->self);
    snx_st_le64(r, (cap + 1) | 1);
    snx_st_le64(r + 8, size);
    snx_st_le64(r + 16, data);
    s->is_long = 1, s->data = data, s->cap = cap, s->size = size;
    H(data)[size] = 0;
}
/* libc++'s __recommend: the capacity of a buffer for n characters, its allocation a multiple of 16 */
static u64 recommend(u64 n) { return n <= SHORT_CAP ? SHORT_CAP : ((n + 16) & ~15ull) - 1; }

/* ---- the guest's operator new and delete, called with the registers kept as the hooked call left them, for a
   handler that then runs the function's own code */
static u64 g_new, g_delete;
static u64 guest_call(CPU* c, u64 fn, u64 a0) {
    u64 x[32], pc = c->pc, r;
    memcpy(x, c->x, sizeof x);
    hook_guest_call(c, fn, a0, 0, 0, 0);
    r = c->x[0];
    memcpy(c->x, x, sizeof x);
    c->pc = pc;
    return r;
}
static u64 buf_new(CPU* c, u64 size) {
    if (!g_new) g_new = kernel_find_symbol_exact("_Znwm");
    return g_new ? guest_call(c, g_new, size) : 0;
}
static void buf_delete(CPU* c, u64 p) {
    if (!g_delete) g_delete = kernel_find_symbol_exact("_ZdlPv");
    if (g_delete && p) guest_call(c, g_delete, p);
}

/* Replaces the n_del characters at pos with n characters: src's (a host pointer, which may point into the string
   itself), or n copies of fill when src is NULL. Its caller checked pos <= size and n_del <= size - pos; -1 for a
   result past max_size or no buffer for it, with the string as it was */
static int splice(CPU* c, Str* s, u64 pos, u64 n_del, const char* src, u64 n, char fill) {
    u64 tail = s->size - pos - n_del, size = s->size - n_del + n;
    char* d = H(s->data);
    if (s->size - n_del > MAX_SIZE - n) return -1;
    if (size <= s->cap) {
        char* tmp = NULL;
        if (src && src + n > d && src < d + s->cap + 1) { /* from the string itself, which the move shifts */
            tmp = (char*)malloc(n ? (size_t)n : 1);
            if (!tmp) return -1;
            memcpy(tmp, src, (size_t)n);
            src = tmp;
        }
        if (tail && n != n_del) memmove(d + pos + n, d + pos + n_del, (size_t)tail);
        if (src)
            memcpy(d + pos, src, (size_t)n);
        else
            memset(d + pos, fill, (size_t)n);
        free(tmp);
        set_size(c, s, size);
        return 0;
    }
    /* a new buffer, at least twice the old (libc++'s __grow_by), filled while the old one still holds src */
    u64 cap = recommend(size > 2 * s->cap ? size : 2 * s->cap), old = s->data;
    int was_long = s->is_long;
    u64 p = buf_new(c, cap + 1);
    if (!p) return -1;
    d = H(old);
    char* q = H(p);
    memcpy(q, d, (size_t)pos);
    if (src)
        memcpy(q + pos, src, (size_t)n);
    else
        memset(q + pos, fill, (size_t)n);
    memcpy(q + pos + n, d + pos + n_del, (size_t)tail);
    set_long(c, s, p, cap, size);
    if (was_long) buf_delete(c, old);
    return 0;
}
/* the string at self made the n characters of src (or n copies of fill), for a constructor: self holds nothing yet */
static int init(CPU* c, u64 self, const char* src, u64 n, char fill, u64 reserve) {
    Str s = {self, 0, SHORT_CAP, self + 1, 0};
    if (n > MAX_SIZE || reserve > MAX_SIZE) return -1;
    if (reserve < n) reserve = n;
    if (reserve > SHORT_CAP) {
        u64 cap = recommend(reserve), p;
        char* tmp = NULL;
        if (src) { /* the source may be another string's buffer, which the guest call leaves alone, but copy it */
            tmp = (char*)malloc(n ? (size_t)n : 1);
            if (!tmp) return -1;
            memcpy(tmp, src, (size_t)n);
        }
        p = buf_new(c, cap + 1);
        if (!p) {
            free(tmp);
            return -1;
        }
        if (src)
            memcpy(H(p), tmp, (size_t)n);
        else
            memset(H(p), fill, (size_t)n);
        free(tmp);
        set_long(c, &s, p, cap, n);
        return 0;
    }
    if (src)
        memmove(H(self + 1), src, (size_t)n);
    else
        memset(H(self + 1), fill, (size_t)n);
    set_size(c, &s, n);
    return 0;
}

/* ---- construction and destruction */
void string_drop(CPU* c, u64 self) {
    Str s = get(c, self);
    if (s.is_long) buf_delete(c, s.data);
}
void string_init(CPU* c, u64 self, u64 src, u64 n) {
    if (init(c, self, H(src), n, 0, 0)) OWN_VOID();
}
void string_init_reserve(CPU* c, u64 self, u64 src, u64 n, u64 reserve) {
    if (init(c, self, H(src), n, 0, reserve)) OWN_VOID();
}
void string_init_fill(CPU* c, u64 self, u64 n, u8 ch) {
    if (init(c, self, NULL, n, (char)ch, 0)) OWN_VOID();
}
void string_init_range(CPU* c, u64 self, u64 first, u64 last) {
    if (last < first || init(c, self, H(first), last - first, 0, 0)) OWN_VOID();
}
void string_from_cstr(CPU* c, u64 self, u64 src) {
    if (init(c, self, H(src), strlen(H(src)), 0, 0)) OWN_VOID();
}
void string_copy(CPU* c, u64 self, u64 str) {
    Str o = get(c, str);
    if (!o.is_long) {
        memcpy(H(self), H(str), 24);
        return;
    }
    if (init(c, self, H(o.data), o.size, 0, 0)) OWN_VOID();
}
void string_copy_alloc(CPU* c, u64 self, u64 str, u64 alloc) {
    (void)alloc;
    string_copy(c, self, str);
}
void string_copy_sub(CPU* c, u64 self, u64 str, u64 pos, u64 n, u64 alloc) {
    Str o = get(c, str);
    (void)alloc;
    if (pos > o.size) OWN_VOID();
    if (n > o.size - pos) n = o.size - pos;
    if (init(c, self, H(o.data) + pos, n, 0, 0)) OWN_VOID();
}

/* ---- assignment */
static u64 assign(CPU* c, u64 self, const char* src, u64 n, char fill) {
    Str s = get(c, self);
    if (n <= s.cap) { /* in place, as libc++'s assign: the capacity stays */
        if (src)
            memmove(H(s.data), src, (size_t)n);
        else
            memset(H(s.data), fill, (size_t)n);
        set_size(c, &s, n);
        return self;
    }
    return splice(c, &s, 0, s.size, src, n, fill) ? NPOS : self;
}
u64 string_assign(CPU* c, u64 self, u64 src, u64 n) {
    u64 r = assign(c, self, H(src), n, 0);
    if (r == NPOS) OWN();
    return r;
}
u64 string_assign_cstr(CPU* c, u64 self, u64 src) {
    u64 r = assign(c, self, H(src), strlen(H(src)), 0);
    if (r == NPOS) OWN();
    return r;
}
u64 string_assign_fill(CPU* c, u64 self, u64 n, u8 ch) {
    u64 r = assign(c, self, NULL, n, (char)ch);
    if (r == NPOS) OWN();
    return r;
}
u64 string_assign_char(CPU* c, u64 self, u8 ch) {
    char one = (char)ch;
    u64 r = assign(c, self, &one, 1, 0);
    if (r == NPOS) OWN();
    return r;
}
u64 string_assign_string(CPU* c, u64 self, u64 str) {
    Str o;
    u64 r;
    if (self == str) return self;
    o = get(c, str);
    r = assign(c, self, H(o.data), o.size, 0);
    if (r == NPOS) OWN();
    return r;
}
u64 string_assign_sub(CPU* c, u64 self, u64 str, u64 pos, u64 n) {
    Str o = get(c, str);
    u64 r;
    if (pos > o.size) OWN();
    if (n > o.size - pos) n = o.size - pos;
    r = assign(c, self, H(o.data) + pos, n, 0);
    if (r == NPOS) OWN();
    return r;
}

/* ---- appending, inserting, erasing and replacing */
u64 string_append(CPU* c, u64 self, u64 src, u64 n) {
    Str s = get(c, self);
    if (splice(c, &s, s.size, 0, H(src), n, 0)) OWN();
    return self;
}
u64 string_append_cstr(CPU* c, u64 self, u64 src) { return string_append(c, self, src, strlen(H(src))); }
u64 string_append_fill(CPU* c, u64 self, u64 n, u8 ch) {
    Str s = get(c, self);
    if (splice(c, &s, s.size, 0, NULL, n, (char)ch)) OWN();
    return self;
}
u64 string_append_sub(CPU* c, u64 self, u64 str, u64 pos, u64 n) {
    Str o = get(c, str), s = get(c, self);
    if (pos > o.size) OWN();
    if (n > o.size - pos) n = o.size - pos;
    if (splice(c, &s, s.size, 0, H(o.data) + pos, n, 0)) OWN();
    return self;
}
void string_push_back(CPU* c, u64 self, u8 ch) {
    Str s = get(c, self);
    char one = (char)ch;
    if (splice(c, &s, s.size, 0, &one, 1, 0)) OWN_VOID();
}
void string_resize(CPU* c, u64 self, u64 n, u8 ch) {
    Str s = get(c, self);
    if (n <= s.size)
        set_size(c, &s, n);
    else if (splice(c, &s, s.size, 0, NULL, n - s.size, (char)ch))
        OWN_VOID();
}
void string_reserve(CPU* c, u64 self, u64 n) {
    Str s = get(c, self);
    if (n > MAX_SIZE) OWN_VOID();
    if (n <= s.cap) return; /* never shrinks: the capacity is the string's own business */
    u64 cap = recommend(n), p = buf_new(c, cap + 1), old = s.data;
    int was_long = s.is_long;
    if (!p) OWN_VOID();
    memcpy(H(p), H(old), (size_t)s.size);
    set_long(c, &s, p, cap, s.size);
    if (was_long) buf_delete(c, old);
}
u64 string_insert(CPU* c, u64 self, u64 pos, u64 src, u64 n) {
    Str s = get(c, self);
    if (pos > s.size || splice(c, &s, pos, 0, H(src), n, 0)) OWN();
    return self;
}
u64 string_insert_cstr(CPU* c, u64 self, u64 pos, u64 src) { return string_insert(c, self, pos, src, strlen(H(src))); }
u64 string_insert_fill(CPU* c, u64 self, u64 pos, u64 n, u8 ch) {
    Str s = get(c, self);
    if (pos > s.size || splice(c, &s, pos, 0, NULL, n, (char)ch)) OWN();
    return self;
}
u64 string_insert_sub(CPU* c, u64 self, u64 pos, u64 str, u64 pos2, u64 n) {
    Str s = get(c, self), o = get(c, str);
    if (pos > s.size || pos2 > o.size) OWN();
    if (n > o.size - pos2) n = o.size - pos2;
    if (splice(c, &s, pos, 0, H(o.data) + pos2, n, 0)) OWN();
    return self;
}
/* insert(const_iterator, char) and insert(const_iterator, first, last): the iterator to the first inserted */
u64 string_insert_at(CPU* c, u64 self, u64 at, u8 ch) {
    Str s = get(c, self);
    char one = (char)ch;
    u64 pos = at - s.data;
    if (pos > s.size || splice(c, &s, pos, 0, &one, 1, 0)) OWN();
    return s.data + pos;
}
u64 string_insert_range(CPU* c, u64 self, u64 at, u64 first, u64 last) {
    Str s = get(c, self);
    u64 pos = at - s.data;
    if (pos > s.size || last < first || splice(c, &s, pos, 0, H(first), last - first, 0)) OWN();
    return s.data + pos;
}
u64 string_erase(CPU* c, u64 self, u64 pos, u64 n) {
    Str s = get(c, self);
    if (pos > s.size) OWN();
    if (n > s.size - pos) n = s.size - pos;
    splice(c, &s, pos, n, NULL, 0, 0); /* shrinks: never fails */
    return self;
}
u64 string_replace(CPU* c, u64 self, u64 pos, u64 n1, u64 src, u64 n2) {
    Str s = get(c, self);
    if (pos > s.size) OWN();
    if (n1 > s.size - pos) n1 = s.size - pos;
    if (splice(c, &s, pos, n1, H(src), n2, 0)) OWN();
    return self;
}
u64 string_replace_cstr(CPU* c, u64 self, u64 pos, u64 n1, u64 src) {
    return string_replace(c, self, pos, n1, src, strlen(H(src)));
}
u64 string_replace_fill(CPU* c, u64 self, u64 pos, u64 n1, u64 n2, u8 ch) {
    Str s = get(c, self);
    if (pos > s.size) OWN();
    if (n1 > s.size - pos) n1 = s.size - pos;
    if (splice(c, &s, pos, n1, NULL, n2, (char)ch)) OWN();
    return self;
}
u64 string_replace_sub(CPU* c, u64 self, u64 pos, u64 n1, u64 str, u64 pos2, u64 n2) {
    Str s = get(c, self), o = get(c, str);
    if (pos > s.size || pos2 > o.size) OWN();
    if (n1 > s.size - pos) n1 = s.size - pos;
    if (n2 > o.size - pos2) n2 = o.size - pos2;
    if (splice(c, &s, pos, n1, H(o.data) + pos2, n2, 0)) OWN();
    return self;
}

/* libc++'s internal growth, which its inlined callers call: a new buffer of at least old_cap + delta_cap, the first
   n_copy characters and the ones after the n_del erased kept, n_add left for the caller (which sets the size); with
   _and_replace, the n_add of src written, and the size set */
static int grow_by(CPU* c, u64 self, u64 old_cap, u64 delta_cap, u64 old_size, u64 n_copy, u64 n_del, u64 n_add,
                   const char* src) {
    Str s = get(c, self);
    u64 old = s.data, want = old_cap + delta_cap, cap, p, rest;
    int was_long = s.is_long;
    char* tmp = NULL;
    if (delta_cap > MAX_SIZE - old_cap || n_copy + n_del > old_size) return -1;
    rest = old_size - n_del - n_copy;
    cap = old_cap < MAX_SIZE / 2 ? recommend(want > 2 * old_cap ? want : 2 * old_cap) : MAX_SIZE - 1;
    if (src) {
        tmp = (char*)malloc(n_add ? (size_t)n_add : 1);
        if (!tmp) return -1;
        memcpy(tmp, src, (size_t)n_add);
    }
    p = buf_new(c, cap + 1);
    if (!p) {
        free(tmp);
        return -1;
    }
    memcpy(H(p), H(old), (size_t)n_copy);
    if (tmp) memcpy(H(p) + n_copy, tmp, (size_t)n_add);
    memcpy(H(p) + n_copy + n_add, H(old) + n_copy + n_del, (size_t)rest);
    free(tmp);
    if (was_long) buf_delete(c, old);
    /* the capacity and data; the size is the caller's to set, except for _and_replace */
    u8* r = hook_host(c, self);
    snx_st_le64(r, (cap + 1) | 1);
    snx_st_le64(r + 16, p);
    if (src) {
        snx_st_le64(r + 8, n_copy + n_add + rest);
        H(p)[n_copy + n_add + rest] = 0;
    }
    return 0;
}
void string_grow_by(CPU* c, u64 self, u64 old_cap, u64 delta_cap, u64 old_size, u64 n_copy, u64 n_del, u64 n_add) {
    if (grow_by(c, self, old_cap, delta_cap, old_size, n_copy, n_del, n_add, NULL)) OWN_VOID();
}
void string_grow_by_and_replace(CPU* c, u64 self, u64 old_cap, u64 delta_cap, u64 old_size, u64 n_copy, u64 n_del,
                                u64 n_add, u64 src) {
    if (grow_by(c, self, old_cap, delta_cap, old_size, n_copy, n_del, n_add, H(src))) OWN_VOID();
}

/* ---- reading */
u64 string_at(CPU* c, u64 self, u64 n) {
    Str s = get(c, self);
    if (n >= s.size) OWN();
    return s.data + n;
}
u64 string_copy_out(CPU* c, u64 self, u64 dst, u64 n, u64 pos) {
    Str s = get(c, self);
    if (pos > s.size) OWN();
    if (n > s.size - pos) n = s.size - pos;
    memmove(H(dst), H(s.data) + pos, (size_t)n);
    return n;
}
static i32 compare(const char* a, u64 na, const char* b, u64 nb) {
    int r = memcmp(a, b, (size_t)(na < nb ? na : nb));
    if (r) return r < 0 ? -1 : 1;
    return na < nb ? -1 : na > nb;
}
i32 string_compare_cstr(CPU* c, u64 self, u64 src) {
    Str s = get(c, self);
    return compare(H(s.data), s.size, H(src), strlen(H(src)));
}
i32 string_compare_sub(CPU* c, u64 self, u64 pos, u64 n1, u64 src, u64 n2) {
    Str s = get(c, self);
    if (pos > s.size || n2 == NPOS) OWN();
    if (n1 > s.size - pos) n1 = s.size - pos;
    return compare(H(s.data) + pos, n1, H(src), n2);
}
i32 string_compare_sub_cstr(CPU* c, u64 self, u64 pos, u64 n1, u64 src) {
    return string_compare_sub(c, self, pos, n1, src, strlen(H(src)));
}
i32 string_compare_sub_sub(CPU* c, u64 self, u64 pos1, u64 n1, u64 str, u64 pos2, u64 n2) {
    Str s = get(c, self), o = get(c, str);
    if (pos1 > s.size || pos2 > o.size) OWN();
    if (n1 > s.size - pos1) n1 = s.size - pos1;
    if (n2 > o.size - pos2) n2 = o.size - pos2;
    return compare(H(s.data) + pos1, n1, H(o.data) + pos2, n2);
}

/* the searches, as libc++'s __str_find and the rest: NPOS for none */
u64 string_find_char(CPU* c, u64 self, u8 ch, u64 pos) {
    Str s = get(c, self);
    const char* d = H(s.data);
    const char* r;
    if (pos >= s.size) return NPOS;
    r = (const char*)memchr(d + pos, ch, (size_t)(s.size - pos));
    return r ? (u64)(r - d) : NPOS;
}
u64 string_rfind_char(CPU* c, u64 self, u8 ch, u64 pos) {
    Str s = get(c, self);
    const char* d = H(s.data);
    u64 i;
    if (!s.size) return NPOS;
    for (i = pos < s.size ? pos + 1 : s.size; i-- > 0;)
        if (d[i] == (char)ch) return i;
    return NPOS;
}
u64 string_find(CPU* c, u64 self, u64 src, u64 pos, u64 n) {
    Str s = get(c, self);
    const char *d = H(s.data), *p = H(src);
    u64 i;
    if (pos > s.size) return NPOS;
    if (!n) return pos;
    for (i = pos; n <= s.size - i; i++)
        if (d[i] == p[0] && !memcmp(d + i, p, (size_t)n)) return i;
    return NPOS;
}
u64 string_rfind(CPU* c, u64 self, u64 src, u64 pos, u64 n) {
    Str s = get(c, self);
    const char *d = H(s.data), *p = H(src);
    u64 i;
    if (n > s.size) return NPOS;
    i = pos < s.size - n ? pos : s.size - n;
    for (;; i--) {
        if (!memcmp(d + i, p, (size_t)n)) return i;
        if (!i) return NPOS;
    }
}
static int in_set(const char* set, u64 n, char ch) { return n && memchr(set, ch, (size_t)n) != NULL; }
static u64 find_of(CPU* c, u64 self, u64 src, u64 pos, u64 n, int want, int last) {
    Str s = get(c, self);
    const char *d = H(s.data), *p = H(src);
    u64 i;
    if (!last) {
        for (i = pos; i < s.size; i++)
            if (in_set(p, n, d[i]) == want) return i;
        return NPOS;
    }
    if (!s.size) return NPOS;
    for (i = pos < s.size ? pos + 1 : s.size; i-- > 0;)
        if (in_set(p, n, d[i]) == want) return i;
    return NPOS;
}
u64 string_find_first_of(CPU* c, u64 self, u64 src, u64 pos, u64 n) {
    return n ? find_of(c, self, src, pos, n, 1, 0) : NPOS;
}
u64 string_find_last_of(CPU* c, u64 self, u64 src, u64 pos, u64 n) {
    return n ? find_of(c, self, src, pos, n, 1, 1) : NPOS;
}
u64 string_find_first_not_of(CPU* c, u64 self, u64 src, u64 pos, u64 n) {
    return find_of(c, self, src, pos, n, 0, 0);
}
u64 string_find_last_not_of(CPU* c, u64 self, u64 src, u64 pos, u64 n) {
    return find_of(c, self, src, pos, n, 0, 1);
}
