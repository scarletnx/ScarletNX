/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* std::string's hooks (src/nx/hle/hook/libcxx_string.c) on libc++'s layout in guest memory: short and long strings,
   growth through the guest's operator new with the old buffer given back to its operator delete, sources that
   point into the string itself, the searches and comparisons, libc++'s __grow_by, and what would throw left to the
   function's own code with the string unchanged */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NEW    0x100u
#define DELETE 0x200u
#define NPOS   (~0ull)

static u8 g_guest[0x40000];
static u64 g_heap = 0x20000;
static int g_news, g_deletes, g_own;
static u64 g_deleted;

u64 kernel_find_symbol_exact(const char* name) {
    return !strcmp(name, "_Znwm") ? NEW : !strcmp(name, "_ZdlPv") ? DELETE : 0;
}
/* operator new: a bump allocator; operator delete: counted. The call clobbers registers, as a guest call would */
void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a1, (void)a2, (void)a3;
    for (int i = 0; i < 31; i++) c->x[i] = 0xDEAD;
    c->pc = 0xBAD;
    if (fn == NEW) {
        c->x[0] = g_heap;
        g_heap += (a0 + 15) & ~15ull;
        g_news++;
    } else if (fn == DELETE) {
        g_deleted = a0;
        g_deletes++;
    }
}
void hook_run_original(CPU* c, u64 addr) {
    assert(addr == 0x4000 && c->pc == 0x4000 && c->x[1] == 0x1111); /* as the hooked call left them */
    g_own++;
    c->x[0] = 0x777;
}

static CPU g_c;
static CPU* c = &g_c;
static u64 put(u64 a, const char* s) {
    memcpy(g_guest + a, s, strlen(s) + 1);
    return a;
}
/* the string at self: its size, its characters and whether they are long, as the game's inlined code reads them */
static void expect(u64 self, const char* want, int is_long) {
    u8* r = g_guest + self;
    size_t n = strlen(want);
    assert((r[0] & 1) == is_long);
    if (is_long) {
        u64 cap = (snx_ld_le64(r) & ~1ull) - 1, size = snx_ld_le64(r + 8), data = snx_ld_le64(r + 16);
        assert(size == n && cap >= n && (cap + 1) % 16 == 0);
        assert(!memcmp(g_guest + data, want, n + 1));
    } else {
        assert((size_t)(r[0] >> 1) == n && !memcmp(r + 1, want, n + 1));
    }
}
static u64 data_of(u64 self) { return g_guest[self] & 1 ? snx_ld_le64(g_guest + self + 16) : self + 1; }
/* the registers of a hooked call: the handler may give it to the function's own code */
static void call(void) {
    c->pc = 0x4000;
    c->x[1] = 0x1111;
}

int main(void) {
    const u64 s = 0x1000, t = 0x1040, u = 0x1080;
    c->mem = g_guest;
    /* short: built, appended to, then long */
    call(), string_init(c, s, put(0x3000, "hello"), 5);
    expect(s, "hello", 0);
    call(), assert(string_append_cstr(c, s, put(0x3000, ", world")) == s);
    expect(s, "hello, world", 0);
    assert(!g_news);
    call(), string_append_cstr(c, s, put(0x3000, " and some more"));
    expect(s, "hello, world and some more", 1);
    assert(g_news == 1 && !g_deletes);
    /* a source inside the string itself, in place and through a new buffer */
    u64 before = data_of(s);
    call(), string_append(c, s, data_of(s), 5);
    expect(s, "hello, world and some morehello", 1);
    assert(data_of(s) == before);
    call(), string_append(c, s, data_of(s), 31);
    expect(s, "hello, world and some morehellohello, world and some morehello", 1);
    assert(g_news == 2 && g_deletes == 1 && g_deleted == before);
    /* insert, erase, replace (one into itself), resize, push_back */
    call(), string_assign_cstr(c, s, put(0x3000, "abcdef"));
    expect(s, "abcdef", 1); /* the capacity stays */
    call(), string_insert_cstr(c, s, 3, put(0x3000, "XY"));
    expect(s, "abcXYdef", 1);
    call(), string_erase(c, s, 1, 2);
    expect(s, "aXYdef", 1);
    call(), string_erase(c, s, 4, NPOS);
    expect(s, "aXYd", 1);
    call(), string_replace(c, s, 1, 2, data_of(s), 4);
    expect(s, "aaXYdd", 1);
    call(), string_replace_fill(c, s, 0, 1, 3, 'z');
    expect(s, "zzzaXYdd", 1);
    call(), string_resize(c, s, 3, 0);
    expect(s, "zzz", 1);
    call(), string_resize(c, s, 5, '!');
    expect(s, "zzz!!", 1);
    call(), string_push_back(c, s, '?');
    expect(s, "zzz!!?", 1);
    /* short ones: assign a char, a fill, insert at an iterator, insert a range */
    call(), string_init(c, t, put(0x3000, "x"), 1);
    call(), string_assign_char(c, t, 'q');
    expect(t, "q", 0);
    call(), string_assign_fill(c, t, 3, 'w');
    expect(t, "www", 0);
    call(), assert(string_insert_at(c, t, data_of(t) + 1, '-') == data_of(t) + 1);
    expect(t, "w-ww", 0);
    put(0x3000, "123");
    call(), assert(string_insert_range(c, t, data_of(t) + 4, 0x3000, 0x3003) == data_of(t) + 4);
    expect(t, "w-ww123", 0);
    /* copies: short is its bytes, long its own buffer; the sub-string constructor */
    int news = g_news;
    call(), string_copy(c, u, t);
    expect(u, "w-ww123", 0);
    assert(g_news == news);
    call(), string_assign_cstr(c, s, put(0x3000, "the quick brown fox jumps over the lazy dog"));
    call(), string_copy(c, u, s);
    expect(u, "the quick brown fox jumps over the lazy dog", 1);
    assert(data_of(u) != data_of(s));
    int deletes = g_deletes;
    call(), string_drop(c, u);
    assert(g_deletes == deletes + 1 && g_deleted == data_of(u));
    call(), string_copy_sub(c, u, s, 4, 5, 0);
    expect(u, "quick", 0);
    call(), string_assign_string(c, u, u);
    expect(u, "quick", 0);
    /* the searches: "the quick brown fox jumps over the lazy dog" */
    assert(string_find_char(c, s, 'q', 0) == 4 && string_find_char(c, s, 'q', 5) == NPOS);
    assert(string_rfind_char(c, s, 'o', NPOS) == 41 && string_rfind_char(c, s, 'o', 40) == 26);
    assert(string_find(c, s, put(0x3000, "the"), 1, 3) == 31 && string_find(c, s, 0x3000, 0, 0) == 0);
    assert(string_find(c, s, 0x3000, 44, 0) == NPOS && string_find(c, s, put(0x3000, "cat"), 0, 3) == NPOS);
    assert(string_rfind(c, s, put(0x3000, "the"), NPOS, 3) == 31 && string_rfind(c, s, 0x3000, 30, 3) == 0);
    assert(string_rfind(c, s, 0x3000, 5, 0) == 5 && string_rfind(c, s, 0x3000, NPOS, 0) == 43);
    assert(string_find_first_of(c, s, put(0x3000, "xyz"), 0, 3) == 18);
    assert(string_find_last_of(c, s, 0x3000, NPOS, 3) == 38 && string_find_first_of(c, s, 0x3000, 0, 0) == NPOS);
    assert(string_find_first_not_of(c, s, put(0x3000, "the "), 0, 4) == 4);
    assert(string_find_last_not_of(c, s, put(0x3000, "dog"), NPOS, 3) == 39);
    /* the comparisons: the sign */
    assert(string_compare_cstr(c, u, put(0x3000, "quick")) == 0 && string_compare_cstr(c, u, put(0x3000, "quicker")) < 0);
    assert(string_compare_cstr(c, u, put(0x3000, "quack")) > 0 && string_compare_cstr(c, u, put(0x3000, "qu")) > 0);
    assert(string_compare_sub(c, s, 4, 5, put(0x3000, "quickly"), 5) == 0);
    assert(string_compare_sub_cstr(c, s, 40, NPOS, put(0x3000, "dog")) == 0);
    assert(string_compare_sub_sub(c, s, 4, 5, u, 0, NPOS) == 0);
    /* at, copy out */
    assert(string_at(c, u, 1) == data_of(u) + 1);
    assert(string_copy_out(c, s, 0x3100, 100, 40) == 3 && !memcmp(g_guest + 0x3100, "dog", 3));
    /* what would throw: the function's own code, with the string as it was */
    call(), assert(string_at(c, u, 5) == 0x777 && g_own == 1);
    call(), assert(string_erase(c, u, 6, 1) == 0x777 && g_own == 2);
    call(), assert(string_insert_cstr(c, u, 9, put(0x3000, "x")) == 0x777 && g_own == 3);
    call(), assert(string_compare_sub(c, u, 0, 1, 0x3000, NPOS) == 0x777 && g_own == 4);
    call(), string_reserve(c, u, 1ull << 63);
    assert(g_own == 5);
    expect(u, "quick", 0);
    /* reserve, then libc++'s __grow_by (the caller sets the size) and __grow_by_and_replace */
    call(), string_reserve(c, u, 40);
    expect(u, "quick", 1);
    assert((snx_ld_le64(g_guest + u) & ~1ull) - 1 >= 40);
    call(), string_assign_cstr(c, t, put(0x3000, "abcdefghij"));
    call(), string_grow_by_and_replace(c, t, 22, 10, 10, 2, 3, 4, put(0x3000, "WXYZ"));
    expect(t, "abWXYZfghij", 1);
    call(), string_grow_by(c, t, 47, 50, 11, 2, 0, 1);
    snx_st_le64(g_guest + t + 8, 12);
    g_guest[data_of(t) + 2] = '+', g_guest[data_of(t) + 12] = 0;
    expect(t, "ab+WXYZfghij", 1);
    puts("PASS std::string's hooks on libc++'s layout");
    return 0;
}
