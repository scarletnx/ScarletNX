/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The header's bytes, every number little-endian:

     0    "SNXSTATE"
     8    u32 format, u32 abi, u64 title_id, u64 created, u64 guest_ns, u32 thumb_w, u32 thumb_h,
          u64 table_off, u32 sections, u32 0, char runtime[96], char stamp[64], char game[128]
     512  the thumbnail: thumb_w * thumb_h RGBA8 pixels */
#include "common/utilities/state_header.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/mods.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void snx_state_header_write(uint8_t b[SNX_STATE_HEADER], const SnxStateHeader* h, uint64_t table_off,
                            uint32_t sections) {
    memset(b, 0, SNX_STATE_HEADER);
    memcpy(b, "SNXSTATE", 8);
    snx_st_le32(b + 8, h->format);
    snx_st_le32(b + 12, h->abi);
    snx_st_le64(b + 16, h->title_id);
    snx_st_le64(b + 24, h->created);
    snx_st_le64(b + 32, h->guest_ns);
    snx_st_le32(b + 40, h->thumb_w);
    snx_st_le32(b + 44, h->thumb_h);
    snx_st_le64(b + 48, table_off);
    snx_st_le32(b + 56, sections);
    memcpy(b + 64, h->runtime, sizeof h->runtime);
    memcpy(b + 160, h->stamp, sizeof h->stamp);
    memcpy(b + 224, h->game, sizeof h->game);
}
int snx_state_header_read(const uint8_t b[SNX_STATE_HEADER], SnxStateHeader* h, uint64_t* table_off,
                          uint32_t* sections, char* why, size_t whysize) {
    if (memcmp(b, "SNXSTATE", 8)) {
        snx_state_why(why, whysize, "it is not a save state");
        return -1;
    }
    *h = (SnxStateHeader){0};
    h->format = snx_ld_le32(b + 8);
    h->abi = snx_ld_le32(b + 12);
    h->title_id = snx_ld_le64(b + 16);
    h->created = snx_ld_le64(b + 24);
    h->guest_ns = snx_ld_le64(b + 32);
    h->thumb_w = snx_ld_le32(b + 40);
    h->thumb_h = snx_ld_le32(b + 44);
    *table_off = snx_ld_le64(b + 48);
    *sections = snx_ld_le32(b + 56);
    memcpy(h->runtime, b + 64, sizeof h->runtime - 1);
    memcpy(h->stamp, b + 160, sizeof h->stamp - 1);
    memcpy(h->game, b + 224, sizeof h->game - 1);
    if (h->format != SNX_STATE_FORMAT) {
        snx_state_why(why, whysize, "it was saved in another layout of the state file");
        return -1;
    }
    if (*sections > SNX_STATE_SECTIONS || h->thumb_w > 4096 || h->thumb_h > 4096) {
        snx_state_why(why, whysize, "its header is damaged");
        return -1;
    }
    return 0;
}
void snx_state_thumb(const uint8_t* rgba, int w, int h, size_t pitch, uint8_t* thumb) {
    const int tw = SNX_STATE_THUMB_W, th = SNX_STATE_THUMB_H;
    int dw = w * th >= h * tw ? tw : w * th / h, dh = w * th >= h * tw ? h * tw / w : th;
    int x0 = (tw - dw) / 2, y0 = (th - dh) / 2;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            uint8_t* to = thumb + ((size_t)y * (size_t)tw + (size_t)x) * 4;
            if (x < x0 || x >= x0 + dw || y < y0 || y >= y0 + dh) {
                to[0] = to[1] = to[2] = 0, to[3] = 255;
                continue;
            }
            const uint8_t* from = rgba + (size_t)((y - y0) * h / dh) * pitch + (size_t)((x - x0) * w / dw) * 4;
            to[0] = from[0], to[1] = from[1], to[2] = from[2], to[3] = 255;
        }
}
void snx_state_slot_name(char* out, size_t size, int slot) {
    if (slot)
        snprintf(out, size, "%d", slot);
    else
        snprintf(out, size, "auto");
}
void snx_state_slot_label(char* out, size_t size, int slot, int start) {
    if (slot)
        snprintf(out, size, "%s %d", start ? "Slot" : "slot", slot);
    else
        snprintf(out, size, "%s auto slot", start ? "The" : "the");
}
void snx_state_slot_when(char* out, size_t size, const SnxStateHeader* h) {
    time_t t = (time_t)h->created;
    struct tm tm;
    uint64_t s = h->guest_ns / 1000000000u;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d  %llu:%02u:%02u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, (unsigned long long)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}
int snx_state_peek(const char* path, SnxStateHeader* h, uint8_t** thumb, char* why, size_t whysize) {
    FILE* f = snx_fopen(path, "rb");
    uint8_t b[SNX_STATE_HEADER];
    uint64_t table;
    uint32_t sections;
    int r = -1;
    if (thumb) *thumb = NULL;
    if (!f) {
        snx_state_why(why, whysize, "it can't be opened");
        return -1;
    }
    if (fread(b, 1, sizeof b, f) != sizeof b)
        snx_state_why(why, whysize, "it is too short to be a save state");
    else if (!snx_state_header_read(b, h, &table, &sections, why, whysize)) {
        size_t size = (size_t)h->thumb_w * h->thumb_h * 4;
        r = 0;
        if (thumb && size && (*thumb = (uint8_t*)malloc(size)) != NULL && fread(*thumb, 1, size, f) != size) {
            free(*thumb);
            *thumb = NULL;
        }
    }
    fclose(f);
    return r;
}
int snx_state_fits(const SnxStateHeader* h, uint64_t title, const char* stamp, char* why, size_t whysize) {
    if (h->title_id != title) {
        snx_state_why(why, whysize, "it is another game's");
        return -1;
    }
    if (h->abi != SNX_ABI) {
        if (why && whysize)
            snprintf(why, whysize, "it was saved by a ScarletNX whose recompiled code differs (SNX_ABI %u, not %u)",
                     (unsigned)h->abi, (unsigned)SNX_ABI);
        return -1;
    }
    if (strcmp(stamp, h->stamp)) {
        snx_state_why(why, whysize, "it was saved with another update or other mods");
        return -1;
    }
    return 0;
}
void snx_state_stamp(const char* program, char* out, size_t size) {
    char* path = snx_format("%s.abi", program);
    size_t got = 0, n;
    char* text = path ? (char*)snx_read_file(path, &got) : NULL;
    free(path);
    n = text ? snx_game_stamp_size(text, got) : 0; /* the made line after it says how the code was made, not what */
    if (n > size - 1) n = size - 1;
    if (n) memcpy(out, text, n);
    free(text);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
}
int snx_state_name(const char* name, char* program, size_t psize, char* slot, size_t ssize) {
    size_t n = strlen(name), ext = strlen(SNX_STATE_EXT);
    const char* dot;
    if (n <= ext || strcmp(name + n - ext, SNX_STATE_EXT)) return 0;
    n -= ext;
    for (dot = name + n; dot > name && dot[-1] != '.'; --dot) continue;
    if (dot == name || dot == name + n || dot - 1 == name) return 0; /* no program, or no slot */
    snprintf(program, psize, "%.*s", (int)(dot - 1 - name), name);
    snprintf(slot, ssize, "%.*s", (int)(name + n - dot), dot);
    return 1;
}

void snx_state_schema_name(uint64_t* h, const char* s) {
    for (; *s; s++) *h = snx_fnv_step(*h, (uint8_t)*s);
    *h = snx_fnv_step(*h, 0x1F);
}
int snx_state_schema_out(uint64_t* h, const char* fmt, ...) {
    va_list ap;
    const char* f;
    int before = 1;
    va_start(ap, fmt);
    for (f = fmt; *f; f++) {
        int longs = 0;
        if (*f == '=') before = 0;
        if (*f != '%') {
            if (before) *h = snx_fnv_step(*h, (uint8_t)*f);
            continue;
        }
        f++;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't') longs += *f++ == 'l';
        switch (*f) {
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (before && s) snx_state_schema_name(h, s);
                break;
            }
            case 'p': (void)va_arg(ap, void*); break;
            case 'f':
            case 'g':
            case 'e': (void)va_arg(ap, double); break;
            case 'c':
            case 'd':
            case 'i':
            case 'u':
            case 'x':
            case 'X':
            case 'o':
                if (longs >= 2)
                    (void)va_arg(ap, long long);
                else if (longs)
                    (void)va_arg(ap, long);
                else
                    (void)va_arg(ap, int);
                break;
            case 0: f--; break;
            default: break;
        }
    }
    va_end(ap);
    return 0;
}
