/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   cfg:u, cfg:s and cfg:i, the system configuration, as applications read it: the config blocks of a console set up
   the way Azahar sets up its own (a New 3DS XL or 3DS XL as the board is, English, in Washington DC), their sizes
   checked as the console checks them, and the region, model and transferable ID cfg:u derives from them. The region
   is the title's, as Azahar's automatic one is: the first its SMDH names that has English, else its first region in
   that region's first language (and that region's country).
   Ported from Azahar core/hle/service/cfg/{cfg.cpp,cfg.h,cfg_defaults.cpp,cfg_u.cpp} (GPL-2.0-or-later, Copyright
   2014-2026 Citra Emulator Project / Azahar Emulator Project) at 49a1e8c, which serves all three names as this does.
   A block Azahar has no default for answers NotFound, as Azahar's does. */
#include "3ds/services/cfg.h"
#include "3ds/memory/ctr_memory.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "common/utilities/users.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

#define CFG_ERR_INVALID_SIZE 0xd90103ecu /* InvalidSize, Config, WrongArgument, Permanent */
#define CFG_ERR_NOT_FOUND    0xd90103fau /* NotFound, Config, WrongArgument, Permanent */
#define CFG_LANGUAGE_EN      1u
enum { MODEL_3DS_XL = 1, MODEL_NEW_3DS_XL = 4 };
/* Azahar's AdjustLanguageInfoBlock: the console's region, its language and country for the title */
typedef struct {
    uint32_t region, language, country;
} Locale;
static Locale locale(const SnxCtrProcess* p) {
    /* each region's first language, whether it has English, and a country of it (JPN, USA, EUR, AUS, CHN, KOR, TWN) */
    static const uint8_t first[7] = {0, 1, 1, 1, 6, 7, 11}, english[7] = {0, 1, 1, 1, 0, 0, 0};
    static const uint8_t country[7] = {1, 49, 110, 65, 160, 136, 128};
    uint32_t lockout = p->region_lockout & 0x7f;
    if (!lockout) lockout = 2; /* unknown: Azahar's USA */
    for (uint32_t r = 0; r < 7; ++r)
        if ((lockout >> r & 1) && english[r]) return (Locale){r, CFG_LANGUAGE_EN, r == 1 ? 49u : country[r]};
    uint32_t r = 0;
    while (!(lockout >> r & 1)) ++r;
    return (Locale){r, first[r], country[r]};
}

/* "Washington DC" and "United States" repeated over the 16 languages' 0x40 UTF-16 code units */
static void location_names(uint8_t* out, const char* name) {
    memset(out, 0, 16 * 0x80);
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; name[j]; ++j) out[i * 0x80 + j * 2] = (uint8_t)name[j];
}
/* the block's default and its size, 0 for a block with none (cfg_defaults.cpp) */
static uint32_t block(SnxCtrProcess* p, uint32_t id, uint8_t out[0x800]) {
    memset(out, 0, 0x800);
    switch (id) {
        case 0x00030001: return 8;             /* UserTimeOffset */
        case 0x00050001: out[1] = 2; return 2; /* BacklightControls: power saving off, brightness 2 */
        case 0x00050005: {                     /* StereoCameraSettings, which fix NaN uniforms in some games */
            static const float settings[8] = {62.0f, 289.0f, 76.80000305175781f, 46.08000183105469f,
                                              10.0f, 5.0f,   55.58000183105469f, 21.56999969482422f};
            for (unsigned i = 0; i < 8; ++i) {
                uint32_t bits;
                memcpy(&bits, &settings[i], 4);
                snx_st_le32(out + i * 4, bits);
            }
            return 32;
        }
        case 0x00050009: return 8;             /* BacklightControlNew3ds */
        case 0x00070001: out[0] = 1; return 1; /* SoundOutputMode: stereo */
        case 0x00090000:                       /* ConsoleUniqueID1 */
        case 0x00090001: return 8;             /* ConsoleUniqueID2 */
        case 0x00090002: return 4;             /* ConsoleUniqueID3 */
        case 0x000a0000: {                     /* Console username: ten UTF-16 units, then two zero words. */
            const SnxUser* user = snx_users_selected();
            /* the nickname is UTF-8 (users.c checks it is), up to ten UTF-16 units here: a character past them, or
               whose surrogate pair would not fit, is left out */
            const unsigned char* name = (const unsigned char*)(user && user->nickname[0] ? user->nickname : "Player");
            unsigned units = 0;
            while (*name && units < 10) {
                unsigned c = *name, follow = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3, k;
                c = follow ? c & (0x3Fu >> follow) : c;
                for (k = 1; k <= follow && name[k]; ++k) c = c << 6 | (name[k] & 0x3Fu);
                name += k;
                if (c >= 0x10000) {
                    if (units > 8) break;
                    c -= 0x10000;
                    out[units * 2] = (uint8_t)(0xD800 + (c >> 10)),
                                out[units * 2 + 1] = (uint8_t)((0xD800 + (c >> 10)) >> 8);
                    units++;
                    c = 0xDC00 + (c & 0x3FF);
                }
                out[units * 2] = (uint8_t)c, out[units * 2 + 1] = (uint8_t)(c >> 8);
                units++;
            }
            return 0x1c;
        }
        case 0x000a0001: out[0] = 11, out[1] = 7; return 2; /* Birthday: November 7th */
        case 0x000a0002: out[0] = (uint8_t)locale(p).language; return 1;
        case 0x000b0000: /* CountryInfo: Washington DC in the United States, else the region's country */
            out[3] = (uint8_t)locale(p).country;
            out[2] = out[3] == 49 ? 2 : 0;
            return 4;
        case 0x000b0001: location_names(out, "United States"); return 0x800;
        case 0x000b0002: location_names(out, "Washington DC"); return 0x800;
        case 0x000b0003: return 4;                               /* LatitudeLongitude */
        case 0x000c0000: return 0xc0;                            /* RestrictedPhotoExchange */
        case 0x000c0001: return 0x14;                            /* CoppacsRestriction */
        case 0x000c0002: return 0x200;                           /* ParentalRestrictionEmail */
        case 0x000d0000: out[0] = 0x7f, out[1] = 0x7f; return 4; /* EULAVersion 127.127 */
        case 0x000e0000: return 1;
        case 0x000f0004: /* ConsoleModel, as the board is */
            out[0] = snx_ctr_memory_profile(p->memory)->model == SNX_CTR_NEW ? MODEL_NEW_3DS_XL : MODEL_3DS_XL;
            return 4;
        case 0x000f0006: return 0x28;                          /* XDeviceToken */
        case 0x00110000: out[0] = 1; return 4;                 /* SystemSetupRequired */
        case 0x00130000: return 4;                             /* DebugMode off */
        case 0x00150000: return 4;                             /* ClockSequence */
        case 0x00150002: out[0] = 'L', out[1] = '1'; return 4; /* ServerType */
        case 0x00160000: return 4;
        case 0x00170000: return 4; /* MiiverseAccessKey */
        default: return 0;
    }
}
/* GetConfig (cfg:u 0x0001) and GetConfigInfoBlk8 (cfg:s and cfg:i 0x0401): size, block, a mapped buffer written */
static void get_config(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 2)) return;
    uint32_t size = c->in[1], id = c->in[2], descriptor = c->in[3], address = c->in[4];
    if ((descriptor & 15) != 12 || (descriptor >> 4) < size) {
        snx_ctr_ipc_bad(c);
        return;
    }
    uint8_t data[0x800];
    uint32_t have = block(c->process, id, data), result = 0;
    if (!have) {
        fprintf(stderr, "3ds cfg: config block 0x%08x has no default\n", (unsigned)id);
        result = CFG_ERR_NOT_FOUND;
    } else if (size != have)
        result = CFG_ERR_INVALID_SIZE;
    else if (snx_ctr_write_ram(c->process->space, address, data, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x42;
    c->out[1] = result;
    c->out[2] = descriptor;
    c->out[3] = address;
}
static void reply(SnxCtrIpc* c, uint32_t value) {
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x80;
    c->out[1] = 0;
    c->out[2] = value;
}
/* GetTransferableId: the last eight bytes of SHA-256 over ConsoleUniqueID2 and the salt's low 20 bits */
static void transferable_id(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 0)) return;
    uint8_t buffer[12], hash[32], data[0x800];
    block(c->process, 0x00090001, data);
    memcpy(buffer, data, 8);
    snx_st_le32(buffer + 8, c->in[1] & 0xfffff);
    if (!EVP_Digest(buffer, sizeof buffer, hash, NULL, EVP_sha256(), NULL)) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0xc0;
    c->out[1] = 0;
    c->out[2] = snx_ld_le32(hash + 24);
    c->out[3] = snx_ld_le32(hash + 28);
}
void snx_ctr_cfg_dispatch(SnxCtrIpc* c) {
    uint8_t data[0x800];
    switch (c->in[0] >> 16) {
        case 0x0001:
        case 0x0401: get_config(c); break;
        case 0x0002: /* GetRegion */
        case 0x0406: /* cfg:s and cfg:i SecureInfoGetRegion */
            if (snx_ctr_ipc_expect(c, 0, 0)) reply(c, locale(c->process).region);
            break;
        case 0x0003: transferable_id(c); break;
        case 0x0004: /* IsCoppacsSupported: in the USA */
            if (snx_ctr_ipc_expect(c, 0, 0)) reply(c, locale(c->process).region == 1);
            break;
        case 0x0005: /* GetSystemModel */
            if (snx_ctr_ipc_expect(c, 0, 0)) reply(c, (block(c->process, 0x000f0004, data), data[0]));
            break;
        case 0x0006: /* GetModelNintendo2DS: 1 for every model but the 2DS */
            if (snx_ctr_ipc_expect(c, 0, 0)) reply(c, 1);
            break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}
