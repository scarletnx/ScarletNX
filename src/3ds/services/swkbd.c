/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The software keyboard, the library applet a game starts to ask the player for a text, played in the applet's
   place. With no box over the game to ask in, it answers at once: swkbd_text, else the game's own initial text, else
   the console's user name, held to what the configuration asks (its length, its input rule, its filters), with OK,
   or with Cancel when the text breaks one of them. Ported from Azahar core/hle/applets/swkbd.{cpp,h} and
   core/frontend/applets/swkbd.cpp (GPL-2.0-or-later, Copyright 2015-2018 Citra Emulator Project / Azahar Emulator
   Project) at ac4a97e: the configuration's layout, its result codes, the Message and Finalize steps, and
   DefaultKeyboard's answer of the user name and its checks. */
#include "3ds/services/swkbd.h"
#include "3ds/memory/ctr_memory.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/apt.h"
#include "3ds/services/cfg.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include <stdio.h>
#include <string.h>

/* SoftwareKeyboardConfig, 0x400 bytes: where its fields are (Azahar applets/swkbd.h, libctru's SwkbdState) */
enum {
    CONFIG_TYPE = 0x00,
    CONFIG_BUTTONS = 0x04, /* num_buttons_m1: 0 OK, 1 Cancel and OK, 2 Cancel, I Forgot and OK, 3 none */
    CONFIG_RULE = 0x08,    /* SoftwareKeyboardValidInput */
    CONFIG_FILTERS = 0x18,
    CONFIG_MAX_LENGTH = 0x20,
    CONFIG_MAX_DIGITS = 0x24,
    CONFIG_LANGUAGE = 0x11e, /* 0: the system's, else a CFG language + 1 */
    CONFIG_INITIAL = 0x120,  /* the initial text's offset in the text block, -1 for none */
    CONFIG_RESULT = 0x138,
    CONFIG_TEXT_OFFSET = 0x144,
    CONFIG_TEXT_LENGTH = 0x148,
    CONFIG_CALLBACK = 0x14c,         /* what the game's own check answered: OK, Close or Continue */
    CONFIG_CALLBACK_MESSAGE = 0x150, /* why, 256 UTF-16 units and a terminator */
    CONFIG_SIZE = 0x400
};
enum { RULE_ANYTHING, RULE_NOT_EMPTY, RULE_NOT_EMPTY_NOT_BLANK, RULE_NOT_BLANK, RULE_FIXED_LENGTH };
enum { FILTER_DIGITS = 1, FILTER_AT = 2, FILTER_PERCENT = 4, FILTER_BACKSLASH = 8, FILTER_CALLBACK = 0x20 };
enum { CALLBACK_OK, CALLBACK_CLOSE, CALLBACK_CONTINUE };
#define SIGNAL_WAKEUP_BY_CANCEL 0xcu        /* CancelLibraryApplet's: the applet closes */
#define RESULT_NONE             0xffffffffu /* no button: NoButton's answer */
#define RESULT_BANNED           30u         /* BannedInput: the game's own check closed the keyboard */
#define TEXT_UNITS              2048u       /* the most of a text the keyboard answers, in UTF-16 units */
#define DEFAULT_MAX_LENGTH      0xfde8u     /* a max_text_length of 0, as libctru's swkbdInit makes it */

bool snx_ctr_swkbd_is(uint32_t applet_id) { return applet_id == 0x201 || applet_id == 0x401; }

/* UTF-8 to UTF-16, up to most units (a pair is never split); a byte that starts no character, a sequence cut
   short, an overlong form or a surrogate each become one U+FFFD. Returns the units */
static uint32_t utf8_to_16(const char* in, uint16_t* out, uint32_t most) {
    const unsigned char* s = (const unsigned char*)in;
    uint32_t n = 0;
    while (*s) {
        uint32_t c = *s++, need = 0;
        if (c >= 0x80) {
            need = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
            if (!need)
                c = 0xfffd;
            else {
                uint32_t i = 0;
                c &= 0x3fu >> need;
                while (i < need && (s[i] & 0xc0) == 0x80) c = c << 6 | (s[i++] & 0x3fu);
                s += i;
                if (i < need || (need == 2 && c < 0x800) || (need == 3 && (c < 0x10000 || c > 0x10ffff)) ||
                    (c >= 0xd800 && c < 0xe000))
                    c = 0xfffd;
            }
        }
        if (n + (c >= 0x10000 ? 2 : 1) > most) break;
        if (c >= 0x10000) {
            out[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
        } else
            out[n++] = (uint16_t)c;
    }
    return n;
}
/* UTF-16 (n units) to UTF-8 for a log line, cut to cap with its terminator; an unpaired surrogate is U+FFFD */
static void utf16_to_8(const uint16_t* in, uint32_t n, char* out, size_t cap) {
    size_t k = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t c = in[i];
        unsigned char b[4];
        size_t len;
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && in[i + 1] >= 0xdc00 && in[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (in[++i] - 0xdc00u);
        else if (c >= 0xd800 && c < 0xe000)
            c = 0xfffd;
        if (c < 0x80)
            b[0] = (unsigned char)c, len = 1;
        else if (c < 0x800)
            b[0] = (unsigned char)(0xc0 | c >> 6), b[1] = (unsigned char)(0x80 | (c & 0x3f)), len = 2;
        else if (c < 0x10000)
            b[0] = (unsigned char)(0xe0 | c >> 12), b[1] = (unsigned char)(0x80 | (c >> 6 & 0x3f)),
            b[2] = (unsigned char)(0x80 | (c & 0x3f)), len = 3;
        else
            b[0] = (unsigned char)(0xf0 | c >> 18), b[1] = (unsigned char)(0x80 | (c >> 12 & 0x3f)),
            b[2] = (unsigned char)(0x80 | (c >> 6 & 0x3f)), b[3] = (unsigned char)(0x80 | (c & 0x3f)), len = 4;
        if (k + len >= cap) break;
        memcpy(out + k, b, len);
        k += len;
    }
    out[k] = 0;
}
/* the text block's bytes at offset, read or written: the application's own range for a block it made, else the
   block's pages. False when they are not all the block's or its memory refuses them */
static bool text_block(SnxCtrProcess* p, uint32_t offset, void* bytes, uint32_t size, bool write) {
    const CtrObject* o = &p->objects[p->swkbd.text];
    if (offset > o->data.shared.size || size > o->data.shared.size - offset) return false;
    if (o->data.shared.by_application) {
        uint32_t va = o->data.shared.address + offset;
        return (write ? snx_ctr_write_ram(p->space, va, bytes, size) : snx_ctr_read_ram(p->space, va, bytes, size)) ==
               SNX_CTR_MEM_OK;
    }
    uint32_t pa = o->data.shared.physical + offset;
    return (write ? snx_ctr_phys_write(p->memory, pa, bytes, size) : snx_ctr_phys_read(p->memory, pa, bytes, size)) ==
           SNX_CTR_MEM_OK;
}
/* the most units the keyboard answers: the game's max_text_length, and what the block holds ahead of a terminator */
static uint32_t most_units(SnxCtrProcess* p) {
    uint32_t most = snx_ld_le16(p->swkbd.config + CONFIG_MAX_LENGTH), room = p->objects[p->swkbd.text].data.shared.size;
    if (!most) most = DEFAULT_MAX_LENGTH;
    room = room / 2 ? room / 2 - 1 : 0;
    if (most > room) most = room;
    return most < TEXT_UNITS - 1 ? most : TEXT_UNITS - 1;
}
/* the game's initial text, up to most units and its terminator: 0 units for none */
static uint32_t initial_text(SnxCtrProcess* p, uint32_t most, uint16_t* out) {
    uint32_t offset = snx_ld_le32(p->swkbd.config + CONFIG_INITIAL), size = p->objects[p->swkbd.text].data.shared.size;
    unsigned char bytes[TEXT_UNITS * 2];
    if (offset == 0xffffffffu || offset >= size) return 0;
    uint32_t take = (most + 1) * 2 < size - offset ? (most + 1) * 2 : (size - offset) & ~1u, n = 0;
    if (!take || !text_block(p, offset, bytes, take, false)) return 0;
    while (n < take / 2 && n < most && snx_ld_le16(bytes + n * 2)) {
        out[n] = snx_ld_le16(bytes + n * 2);
        ++n;
    }
    return n;
}
static bool blank(const uint16_t* t, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (t[i] != 0x20 && t[i] != 0x3000 && (t[i] < 9 || t[i] > 13)) return false;
    return true;
}
/* why the text breaks the configuration's input rule or one of its filters, written to why; false when it breaks
   none (Azahar's ValidateInput and ValidateFilters, but for the profanity filter, which no list here checks) */
static bool broken(const unsigned char* config, const uint16_t* t, uint32_t n, uint32_t most, char* why, size_t size) {
    uint32_t rule = snx_ld_le32(config + CONFIG_RULE), filters = snx_ld_le32(config + CONFIG_FILTERS);
    uint32_t digits = 0, most_digits = snx_ld_le16(config + CONFIG_MAX_DIGITS), at = 0, percent = 0, backslash = 0;
    for (uint32_t i = 0; i < n; ++i) {
        digits += t[i] >= '0' && t[i] <= '9';
        at += t[i] == '@';
        percent += t[i] == '%';
        backslash += t[i] == '\\';
    }
    if ((filters & FILTER_DIGITS) && digits > most_digits)
        snprintf(why, size, "has %u digits, the game takes %u at most", (unsigned)digits, (unsigned)most_digits);
    else if ((filters & FILTER_AT) && at)
        snprintf(why, size, "has an @, which the game refuses");
    else if ((filters & FILTER_PERCENT) && percent)
        snprintf(why, size, "has a %%, which the game refuses");
    else if ((filters & FILTER_BACKSLASH) && backslash)
        snprintf(why, size, "has a \\, which the game refuses");
    else if (rule == RULE_FIXED_LENGTH && n != most)
        snprintf(why, size, "is %u characters, the game asks for exactly %u", (unsigned)n, (unsigned)most);
    else if ((rule == RULE_NOT_EMPTY || rule == RULE_NOT_EMPTY_NOT_BLANK) && !n)
        snprintf(why, size, "is empty, which the game refuses");
    else if ((rule == RULE_NOT_BLANK || rule == RULE_NOT_EMPTY_NOT_BLANK) && n && blank(t, n))
        snprintf(why, size, "is only spaces, which the game refuses");
    else
        return false;
    return true;
}
/* the result codes of the buttons: the rightmost, which submits the text (D0Click, D1Click1, D2Click2, None), and
   the leftmost, Cancel (D1Click0, D2Click0), none with fewer than two */
static uint32_t ok_result(uint32_t buttons) { return buttons == 3 ? RESULT_NONE : buttons == 0 ? 0 : buttons * 3 - 1; }
static bool cancel_result(uint32_t buttons, uint32_t* result) {
    if (buttons != 1 && buttons != 2) return false;
    *result = buttons == 1 ? 1 : 3;
    return true;
}
/* the configuration's answer: the button, the text's length and its place, the block's start, where the text goes
   with its terminator. False when the block refuses the text */
static bool put_answer(SnxCtrProcess* p, const uint16_t* t, uint32_t n, uint32_t result) {
    unsigned char bytes[TEXT_UNITS * 2];
    snx_st_le32(p->swkbd.config + CONFIG_RESULT, result);
    snx_st_le32(p->swkbd.config + CONFIG_TEXT_OFFSET, 0);
    snx_st_le16(p->swkbd.config + CONFIG_TEXT_LENGTH, (uint16_t)n);
    for (uint32_t i = 0; i < n; ++i) snx_st_le16(bytes + i * 2, t[i]);
    snx_st_le16(bytes + n * 2, 0);
    return text_block(p, 0, bytes, (n + 1) * 2, true);
}
/* the keyboard closes: the application wakes with the configuration as it now is (Finalize, CloseLibraryApplet) */
static void finish(SnxCtrProcess* p) {
    uint32_t applet = p->apt_library;
    snx_ctr_object_release(p, p->swkbd.text);
    p->swkbd.text = 0;
    snx_ctr_apt_library_closed(p);
    snx_ctr_apt_send(p, applet, SNX_CTR_APT_WAKEUP_BY_EXIT, 0, p->swkbd.config, CONFIG_SIZE);
}
/* the answer in the player's place, written to the block and the configuration; false when the block refuses it */
static bool answer(SnxCtrProcess* p) {
    uint16_t text[TEXT_UNITS];
    uint32_t most = most_units(p), all, n, buttons = snx_ld_le32(p->swkbd.config + CONFIG_BUTTONS), result;
    char shown[TEXT_UNITS * 3 + 1], what[TEXT_UNITS * 3 + 64], why[96];
    const char* where;
    if (g_settings.keyboard_text[0]) {
        all = utf8_to_16(g_settings.keyboard_text, text, TEXT_UNITS - 1);
        where = "swkbd_text";
    } else if ((all = initial_text(p, TEXT_UNITS - 1, text)) != 0)
        where = "the game's text";
    else {
        all = snx_ctr_cfg_username(text);
        where = NULL;
    }
    n = all < most ? all : most;
    /* the user name stays out of the log, which players attach to public reports: its length stands for it */
    if (!where)
        snprintf(what, sizeof what, "the console's user name (%u characters)", (unsigned)n);
    else {
        utf16_to_8(text, n, shown, sizeof shown);
        snprintf(what, sizeof what, "\"%s\" (%s)", shown, where);
    }
    if (n < all)
        fprintf(stderr, "3ds swkbd: %s is cut from %u characters to the game's %u\n", where ? where : "the user name",
                (unsigned)all, (unsigned)most);
    if (broken(p->swkbd.config, text, n, most, why, sizeof why)) {
        if (cancel_result(buttons, &result)) {
            fprintf(stderr, "3ds swkbd: the software keyboard is cancelled: %s %s\n", what, why);
            return put_answer(p, NULL, 0, result);
        }
        fprintf(stderr, "3ds swkbd: %s %s, and the game's keyboard has no Cancel: it answers it\n", what, why);
    }
    fprintf(stderr, "3ds swkbd: the software keyboard answers %s with %s, as no box asks the player\n", what,
            buttons == 3 ? "no button" : "OK");
    return put_answer(p, text, n, ok_result(buttons));
}
void snx_ctr_swkbd_start(SnxCtrIpc* c, const unsigned char* config, uint32_t size, uint32_t text) {
    SnxCtrProcess* p = c->process;
    uint32_t buttons = size >= 8 ? snx_ld_le32(config + CONFIG_BUTTONS) : 0;
    uint32_t rule = size >= 12 ? snx_ld_le32(config + CONFIG_RULE) : 0;
    if (size != CONFIG_SIZE || !text || p->objects[text].type != CTR_OBJ_SHARED || buttons > 3 ||
        rule > RULE_FIXED_LENGTH) {
        fprintf(stderr,
                "3ds swkbd: the game starts the software keyboard with a %u-byte configuration (buttons %u, input "
                "rule %u) and %s: it takes 0x400 bytes, buttons 0 to 3, rules 0 to 4 and a memory block\n",
                (unsigned)size, (unsigned)buttons, (unsigned)rule,
                !text                                     ? "no text block"
                : p->objects[text].type != CTR_OBJ_SHARED ? "an object that is no memory block"
                                                          : "a memory block");
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    memcpy(p->swkbd.config, config, CONFIG_SIZE);
    p->swkbd.text = text;
    ++p->objects[text].refs; /* the keyboard writes to it until it closes */
    fprintf(
        stderr,
        "3ds swkbd: the game opens the software keyboard: type %u, buttons %u, input rule %u, filters 0x%x, up to %u "
        "characters, language %u (0: the system's)\n",
        (unsigned)snx_ld_le32(config + CONFIG_TYPE), (unsigned)buttons, (unsigned)rule,
        (unsigned)snx_ld_le32(config + CONFIG_FILTERS), (unsigned)most_units(p),
        (unsigned)snx_ld_le16(config + CONFIG_LANGUAGE));
    if (!answer(p)) {
        fprintf(stderr, "3ds swkbd: the game's text block at 0x%08x (%u bytes) refuses the keyboard's text\n",
                (unsigned)p->objects[text].data.shared.address, (unsigned)p->objects[text].data.shared.size);
        snx_ctr_object_release(p, p->swkbd.text);
        p->swkbd.text = 0;
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    /* a game that checks the text itself gets it first, as the console's keyboard sends it when OK is pressed */
    if ((snx_ld_le32(config + CONFIG_FILTERS) & FILTER_CALLBACK) &&
        snx_ld_le32(p->swkbd.config + CONFIG_RESULT) == ok_result(buttons)) {
        snx_ctr_apt_send(p, p->apt_library, SNX_CTR_APT_MESSAGE, 0, p->swkbd.config, CONFIG_SIZE);
        return;
    }
    finish(p);
}
void snx_ctr_swkbd_parameter(SnxCtrIpc* c, uint32_t signal, const unsigned char* buffer, uint32_t size,
                             uint32_t object) {
    SnxCtrProcess* p = c->process;
    (void)object; /* neither a Message nor a cancel carries one */
    if (signal == SNX_CTR_APT_MESSAGE && size == CONFIG_SIZE && p->swkbd.text) {
        uint16_t why[257];
        char shown[257 * 3 + 1];
        uint32_t n = 0, check = snx_ld_le32(buffer + CONFIG_CALLBACK);
        memcpy(p->swkbd.config, buffer, CONFIG_SIZE);
        while (n < 256 && (why[n] = snx_ld_le16(buffer + CONFIG_CALLBACK_MESSAGE + n * 2)) != 0) ++n;
        utf16_to_8(why, n, shown, sizeof shown);
        if (check == CALLBACK_OK)
            fprintf(stderr, "3ds swkbd: the game's own check takes the text\n");
        else if (check == CALLBACK_CLOSE || check == CALLBACK_CONTINUE) {
            /* Continue would show its message and ask again: with no box to ask in, Cancel answers where the
               keyboard has one, else it closes as Close does */
            uint32_t result = RESULT_BANNED;
            if (check == CALLBACK_CONTINUE) (void)cancel_result(snx_ld_le32(p->swkbd.config + CONFIG_BUTTONS), &result);
            fprintf(stderr, "3ds swkbd: the game's own check refuses the text (\"%s\") and %s: the keyboard %s\n",
                    shown, check == CALLBACK_CLOSE ? "closes the keyboard" : "would ask again",
                    result == RESULT_BANNED ? "closes with BannedInput (30)" : "is cancelled");
            if (!put_answer(p, NULL, 0, result))
                fprintf(stderr, "3ds swkbd: the game's text block at 0x%08x refuses the empty answer\n",
                        (unsigned)p->objects[p->swkbd.text].data.shared.address);
        } else {
            fprintf(stderr, "3ds swkbd: the game's own check answers %u, not OK (0), Close (1) or Continue (2)\n",
                    (unsigned)check);
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        finish(p);
        return;
    }
    if (signal == SIGNAL_WAKEUP_BY_CANCEL && p->swkbd.text) {
        fprintf(stderr, "3ds swkbd: the game cancels the software keyboard\n");
        finish(p);
        return;
    }
    fprintf(stderr,
            "3ds swkbd: the game sends the software keyboard signal %u with %u bytes, which it does not take (a "
            "Message with its 0x400-byte configuration, or a cancel while it runs)\n",
            (unsigned)signal, (unsigned)size);
    c->state = SNX_CTR_IPC_UNSUPPORTED;
}
