/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/input/map.h"
#include "3ds/input/input.h"
#include <SDL3/SDL_keyboard.h>
#include <stdio.h>
#include <string.h>

static const struct {
    const char* name;
    uint32_t bit;
} BINDS[SNX_CTR_BINDS] = {
    {"a", SNX_CTR_PAD_A},           {"b", SNX_CTR_PAD_B},         {"x", SNX_CTR_PAD_X},
    {"y", SNX_CTR_PAD_Y},           {"l", SNX_CTR_PAD_L},         {"r", SNX_CTR_PAD_R},
    {"zl", SNX_CTR_PAD_ZL},         {"zr", SNX_CTR_PAD_ZR},       {"start", SNX_CTR_PAD_START},
    {"select", SNX_CTR_PAD_SELECT}, {"up", SNX_CTR_PAD_UP},       {"down", SNX_CTR_PAD_DOWN},
    {"left", SNX_CTR_PAD_LEFT},     {"right", SNX_CTR_PAD_RIGHT}, {"cpad_up", 0},
    {"cpad_down", 0},               {"cpad_left", 0},             {"cpad_right", 0},
    {"cstick_up", 0},               {"cstick_down", 0},           {"cstick_left", 0},
    {"cstick_right", 0},
};
/* the keys of the default layout, the Switch games' as well */
static const struct {
    unsigned binding;
    SDL_Scancode keys[2];
} DEFAULT[] = {
    {SNX_CTR_BIND_A, {SDL_SCANCODE_X, SDL_SCANCODE_SPACE}},
    {SNX_CTR_BIND_B, {SDL_SCANCODE_Z}},
    {SNX_CTR_BIND_X, {SDL_SCANCODE_C}},
    {SNX_CTR_BIND_Y, {SDL_SCANCODE_V}},
    {SNX_CTR_BIND_L, {SDL_SCANCODE_Q}},
    {SNX_CTR_BIND_R, {SDL_SCANCODE_E}},
    {SNX_CTR_BIND_ZL, {SDL_SCANCODE_1}},
    {SNX_CTR_BIND_ZR, {SDL_SCANCODE_3}},
    {SNX_CTR_BIND_START, {SDL_SCANCODE_RETURN}},
    {SNX_CTR_BIND_SELECT, {SDL_SCANCODE_BACKSPACE}},
    {SNX_CTR_BIND_UP, {SDL_SCANCODE_UP}},
    {SNX_CTR_BIND_DOWN, {SDL_SCANCODE_DOWN}},
    {SNX_CTR_BIND_LEFT, {SDL_SCANCODE_LEFT}},
    {SNX_CTR_BIND_RIGHT, {SDL_SCANCODE_RIGHT}},
    {SNX_CTR_BIND_CPAD_UP, {SDL_SCANCODE_W}},
    {SNX_CTR_BIND_CPAD_DOWN, {SDL_SCANCODE_S}},
    {SNX_CTR_BIND_CPAD_LEFT, {SDL_SCANCODE_A}},
    {SNX_CTR_BIND_CPAD_RIGHT, {SDL_SCANCODE_D}},
    {SNX_CTR_BIND_CSTICK_UP, {SDL_SCANCODE_I}},
    {SNX_CTR_BIND_CSTICK_DOWN, {SDL_SCANCODE_K}},
    {SNX_CTR_BIND_CSTICK_LEFT, {SDL_SCANCODE_J}},
    {SNX_CTR_BIND_CSTICK_RIGHT, {SDL_SCANCODE_L}},
};

void snx_ctr_keys_default(SnxCtrKeyMap* map) {
    *map = (SnxCtrKeyMap){0};
    for (size_t i = 0; i < sizeof DEFAULT / sizeof *DEFAULT; ++i)
        for (size_t k = 0; k < 2 && DEFAULT[i].keys[k]; ++k)
            map->key[DEFAULT[i].binding][k] = (uint16_t)DEFAULT[i].keys[k];
}
static void say(char* word, size_t size, const char* text, size_t n) {
    if (word && size) snprintf(word, size, "%.*s", (int)n, text);
}
int snx_ctr_keys_read(SnxCtrKeyMap* map, const char* text, char* word, size_t size) {
    SnxCtrKeyMap read = *map;
    while (text && *text) {
        while (*text == ' ' || *text == '\t') ++text;
        size_t n = strcspn(text, " \t");
        if (!n) break;
        const char* colon = memchr(text, ':', n);
        unsigned binding = 0;
        while (colon && binding < SNX_CTR_BINDS &&
               (strlen(BINDS[binding].name) != (size_t)(colon - text) ||
                strncmp(BINDS[binding].name, text, (size_t)(colon - text))))
            ++binding;
        if (!colon || binding == SNX_CTR_BINDS) {
            say(word, size, text, n);
            return -1;
        }
        memset(read.key[binding], 0, sizeof read.key[binding]);
        unsigned held = 0;
        for (const char* k = colon + 1; k < text + n;) {
            size_t length = (size_t)(text + n - k);
            const char* comma = memchr(k, ',', length);
            if (comma) length = (size_t)(comma - k);
            char name[48];
            if (!length || length >= sizeof name) {
                say(word, size, text, n);
                return -1;
            }
            for (size_t i = 0; i < length; ++i) name[i] = k[i] == '_' ? ' ' : k[i];
            name[length] = 0;
            SDL_Scancode code = SDL_GetScancodeFromName(name);
            if (code == SDL_SCANCODE_UNKNOWN || held == SNX_CTR_BIND_KEYS) {
                say(word, size, text, n);
                return -1;
            }
            read.key[binding][held++] = (uint16_t)code;
            k += length + (comma ? 1 : 0);
        }
        text += n;
    }
    *map = read;
    return 0;
}
uint32_t snx_ctr_bind_bit(unsigned binding) { return binding < SNX_CTR_BINDS ? BINDS[binding].bit : 0; }
uint32_t snx_ctr_pad_bit(int button) {
    /* HidNpadButton's bits: A B X Y L3 R3 L R ZL ZR Plus Minus Left Up Right Down */
    static const uint32_t bits[SNX_PAD_BUTTONS] = {
        SNX_CTR_PAD_A,    SNX_CTR_PAD_B,      SNX_CTR_PAD_X,    SNX_CTR_PAD_Y,  0,
        0,                SNX_CTR_PAD_L,      SNX_CTR_PAD_R,    SNX_CTR_PAD_ZL, SNX_CTR_PAD_ZR,
        SNX_CTR_PAD_START, SNX_CTR_PAD_SELECT, SNX_CTR_PAD_LEFT, SNX_CTR_PAD_UP, SNX_CTR_PAD_RIGHT,
        SNX_CTR_PAD_DOWN};
    return button >= 0 && button < SNX_PAD_BUTTONS ? bits[button] : 0;
}
int snx_ctr_pad_map(SnxPadMap* map, const char* text, int swap, char* word, size_t size) {
    SnxPadMap base, own;
    snx_pad_map_position(&base);
    snx_pad_map_unset(&own);
    int bad = text && *text ? snx_pad_map_read(&own, text, word, size) : 0;
    if (bad) snx_pad_map_unset(&own);
    snx_pad_map_fill(&own, &base);
    if (swap) snx_pad_map_swap_ab(&own);
    *map = own;
    return bad ? -1 : 0;
}
