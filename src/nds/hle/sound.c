/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2016-2022 melonDS team
   Copyright 2026 the ScarletNX contributors */
/* The sound driver the SDK's ARM7 program runs, in the runtime: the game's sound library (the ARM9's side) sends it
   lists of commands over the FIFO and shares a block of main memory with it, and the driver plays sequences (SSEQ)
   with their banks and waves through the sound hardware's sixteen channels (audio/spu.c), every 5.2 ms. Ported from
   melonDS's high-level ARM7 (HLE_Retail/Sound_Nitro.cpp of its melonHLE branch at b42e2f4, GPL-3.0-or-later): the
   commands, the sequence player with its tracks and note operations, the channels' envelopes, sweeps and modulation,
   and the volume and pitch tables, which are the SDK's own numbers.

   The driver's state is this file's: a process runs one board. */
#include "nds/hle/pxi.h"
#include "nds/audio/spu.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct NdsSound {
    int unused;
};

typedef struct SndChannel SndChannel;
typedef struct SndSequence SndSequence;
typedef struct SndTrack SndTrack;
typedef struct SndAlarm SndAlarm;

static NdsBoard* B; /* the board the driver serves */
static uint64_t alarm_due[8];

static uint8_t rd8(uint32_t a) { return nds_read8(B, a); }
static uint16_t rd16(uint32_t a) { return nds_read16(B, a); }
static uint32_t rd32(uint32_t a) { return nds_read32(B, a); }
/* The driver ran on the ARM7, where a write below main memory went to the BIOS and did nothing. A game that gives no
   shared work area makes such writes (to address 0), and on the ARM9 side that place is the ITCM */
static bool arm7_ram(uint32_t a) { return a >= 0x02000000u; }
static void wr8(uint32_t a, uint8_t v) { if (arm7_ram(a)) nds_write8(B, a, v); }
static void wr16(uint32_t a, uint16_t v) { if (arm7_ram(a)) nds_write16(B, a, v); }
static void wr32(uint32_t a, uint32_t v) { if (arm7_ram(a)) nds_write32(B, a, v); }
static uint32_t spu_read(unsigned bytes, uint32_t a) { return nds_spu_read(B, a, bytes); }
static void spu_write(unsigned bytes, uint32_t a, uint32_t v) { nds_spu_write(B, a, bytes, v); }
static void note(const char* format, ...) {
    va_list args;
    if (!B->trace) return;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}
/* an alarm due at a cycle, UINT64_MAX for never: the board's event is the first of them */
static void alarm_set(uint32_t num, uint64_t at) {
    uint64_t first = UINT64_MAX;
    alarm_due[num & 7] = at;
    for (unsigned i = 0; i < 8; ++i)
        if (alarm_due[i] < first) first = alarm_due[i];
    nds_schedule(B, NDS_EVENT_ALARM, first);
}


struct SndChannel
{
    uint8_t ID;
    uint8_t type;
    uint8_t vol_ramp_phase;
    uint8_t status_flags;
    uint8_t pan_base1;
    uint8_t freq_base1;
    int16_t vol_base1;
    uint8_t freq_base2;
    uint8_t vol_base2;
    int8_t pan_base2;
    int8_t pan_base3;
    int16_t vol_base3;
    int16_t freq_base3;
    int32_t base_volume;
    int32_t freq_ramp_pos;
    int32_t freq_ramp_len;
    uint8_t attack_rate;
    uint8_t sustain_rate;
    uint16_t decay_rate;
    uint16_t release_rate;
    uint8_t priority;
    uint8_t pan;
    uint8_t volume;
    uint8_t volume_div;
    uint16_t frequency;
    uint8_t modulation_type;
    uint8_t modulation_speed;
    uint8_t modulation_depth;
    uint8_t modulation_range;
    uint16_t modulation_delay;
    uint16_t modulation_count1;
    uint16_t modulation_count2;
    int16_t freq_ramp_target;
    int32_t note_length;
    uint8_t data_format;
    uint8_t repeat;
    uint16_t sample_rate;
    uint16_t swav_frequency;
    uint16_t loop_pos;
    uint32_t length;
    uint32_t data_addr;
    bool linked;
    SndTrack* linked_track;
    SndChannel* next;
};

struct SndSequence
{
    uint8_t status_flags;
    uint8_t ID;
    uint8_t seq_unk02;
    uint8_t seq_unk03;
    uint8_t pan;
    uint8_t volume;
    int16_t seq_unk06;
    uint8_t tracks[16];
    uint16_t tempo;
    uint16_t seq_unk1_a;
    uint16_t tick_counter;
    uint32_t sbnk_addr;
};

struct SndTrack
{
    uint8_t status_flags;
    uint8_t track_unk01;
    uint16_t instr_index; // index into SBNK
    uint8_t volume;
    uint8_t expression;
    int8_t pitch_bend;
    uint8_t pitch_bend_range;
    int8_t pan;
    int8_t track_unk09;
    int16_t track_unk0_a;
    int16_t frequency;
    uint8_t attack_rate;
    uint8_t decay_rate;
    uint8_t sustain_rate;
    uint8_t release_rate;
    uint8_t priority;
    int8_t transpose;
    int8_t track_unk14;
    uint8_t portamento_time;
    int16_t sweep_pitch;
    uint8_t modulation_type;
    uint8_t modulation_speed;
    uint8_t modulation_depth;
    uint8_t modulation_range;
    uint16_t modulation_delay;
    uint16_t channel_mask;
    int32_t rest_counter;
    uint32_t note_buffer;
    uint32_t cur_note_addr;
    uint32_t loop_addr[3];
    uint8_t loop_count[3];
    uint8_t loop_level;
    SndChannel* chan_list;
};

struct SndAlarm
{
    bool active;
    uint32_t delay;
    uint32_t repeat;
    uint32_t param;
};

static const int16_t base_volume_table[128] =
{
    -0x8000, -0x02D2, -0x02D1, -0x028B, -0x0259, -0x0232, -0x0212, -0x01F7,
    -0x01E0, -0x01CC, -0x01BA, -0x01A9, -0x019A, -0x018C, -0x017F, -0x0173,
    -0x0168, -0x015D, -0x0153, -0x014A, -0x0141, -0x0139, -0x0131, -0x0129,
    -0x0121, -0x011A, -0x0114, -0x010D, -0x0107, -0x0101, -0x00FB, -0x00F5,
    -0x00EF, -0x00EA, -0x00E5, -0x00E0, -0x00DB, -0x00D6, -0x00D2, -0x00CD,
    -0x00C9, -0x00C4, -0x00C0, -0x00BC, -0x00B8, -0x00B4, -0x00B0, -0x00AD,
    -0x00A9, -0x00A5, -0x00A2, -0x009E, -0x009B, -0x0098, -0x0095, -0x0091,
    -0x008E, -0x008B, -0x0088, -0x0085, -0x0082, -0x007F, -0x007D, -0x007A,
    -0x0077, -0x0074, -0x0072, -0x006F, -0x006D, -0x006A, -0x0067, -0x0065,
    -0x0063, -0x0060, -0x005E, -0x005B, -0x0059, -0x0057, -0x0055, -0x0052,
    -0x0050, -0x004E, -0x004C, -0x004A, -0x0048, -0x0046, -0x0044, -0x0042,
    -0x0040, -0x003E, -0x003C, -0x003A, -0x0038, -0x0036, -0x0034, -0x0032,
    -0x0031, -0x002F, -0x002D, -0x002B, -0x002A, -0x0028, -0x0026, -0x0024,
    -0x0023, -0x0021, -0x001F, -0x001E, -0x001C, -0x001B, -0x0019, -0x0017,
    -0x0016, -0x0014, -0x0013, -0x0011, -0x0010, -0x000E, -0x000D, -0x000B,
    -0x000A, -0x0008, -0x0007, -0x0006, -0x0004, -0x0003, -0x0001,  0x0000
};

static const int8_t modulation_table[32] =
{
    0x00, 0x06, 0x0C, 0x13, 0x19, 0x1F, 0x25, 0x2B, 0x31, 0x36, 0x3C, 0x41, 0x47, 0x4C, 0x51, 0x55,
    0x5A, 0x5E, 0x62, 0x66, 0x6A, 0x6D, 0x70, 0x73, 0x75, 0x78, 0x7A, 0x7B, 0x7D, 0x7E, 0x7E, 0x7F
};

static const uint8_t swi_volume_table[724] =
{
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09,
    0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
    0x0B, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0E,
    0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15, 0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x18,
    0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19, 0x1A, 0x1A, 0x1A, 0x1B, 0x1B, 0x1B, 0x1C, 0x1C, 0x1C,
    0x1D, 0x1D, 0x1D, 0x1E, 0x1E, 0x1E, 0x1F, 0x1F, 0x1F, 0x20, 0x20, 0x20, 0x21, 0x21, 0x22, 0x22,
    0x22, 0x23, 0x23, 0x24, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27, 0x27, 0x28, 0x28, 0x29,
    0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E, 0x2F, 0x2F, 0x30, 0x31, 0x31,
    0x32, 0x32, 0x33, 0x33, 0x34, 0x35, 0x35, 0x36, 0x36, 0x37, 0x38, 0x38, 0x39, 0x3A, 0x3A, 0x3B,
    0x3C, 0x3C, 0x3D, 0x3E, 0x3F, 0x3F, 0x40, 0x41, 0x42, 0x42, 0x43, 0x44, 0x45, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x52, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x67,
    0x68, 0x69, 0x6A, 0x6B, 0x6D, 0x6E, 0x6F, 0x71, 0x72, 0x73, 0x75, 0x76, 0x77, 0x79, 0x7A, 0x7B,
    0x7D, 0x7E, 0x7F, 0x20, 0x21, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25,
    0x26, 0x26, 0x26, 0x27, 0x27, 0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D,
    0x2D, 0x2E, 0x2E, 0x2F, 0x2F, 0x30, 0x30, 0x31, 0x31, 0x32, 0x33, 0x33, 0x34, 0x34, 0x35, 0x36,
    0x36, 0x37, 0x37, 0x38, 0x39, 0x39, 0x3A, 0x3B, 0x3B, 0x3C, 0x3D, 0x3E, 0x3E, 0x3F, 0x40, 0x40,
    0x41, 0x42, 0x43, 0x43, 0x44, 0x45, 0x46, 0x47, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4D,
    0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D,
    0x5E, 0x5F, 0x60, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6F, 0x70,
    0x71, 0x73, 0x74, 0x75, 0x77, 0x78, 0x79, 0x7B, 0x7C, 0x7E, 0x7E, 0x40, 0x41, 0x42, 0x43, 0x43,
    0x44, 0x45, 0x46, 0x47, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51,
    0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61,
    0x62, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6B, 0x6C, 0x6D, 0x6E, 0x70, 0x71, 0x72, 0x74, 0x75,
    0x76, 0x78, 0x79, 0x7B, 0x7C, 0x7D, 0x7E, 0x40, 0x41, 0x42, 0x42, 0x43, 0x44, 0x45, 0x46, 0x46,
    0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x65, 0x66,
    0x67, 0x68, 0x69, 0x6A, 0x6C, 0x6D, 0x6E, 0x6F, 0x71, 0x72, 0x73, 0x75, 0x76, 0x77, 0x79, 0x7A,
    0x7C, 0x7D, 0x7E, 0x7F
};

static const uint16_t swi_pitch_table[768] =
{
    0x0000, 0x003B, 0x0076, 0x00B2, 0x00ED, 0x0128, 0x0164, 0x019F,
    0x01DB, 0x0217, 0x0252, 0x028E, 0x02CA, 0x0305, 0x0341, 0x037D,
    0x03B9, 0x03F5, 0x0431, 0x046E, 0x04AA, 0x04E6, 0x0522, 0x055F,
    0x059B, 0x05D8, 0x0614, 0x0651, 0x068D, 0x06CA, 0x0707, 0x0743,
    0x0780, 0x07BD, 0x07FA, 0x0837, 0x0874, 0x08B1, 0x08EF, 0x092C,
    0x0969, 0x09A7, 0x09E4, 0x0A21, 0x0A5F, 0x0A9C, 0x0ADA, 0x0B18,
    0x0B56, 0x0B93, 0x0BD1, 0x0C0F, 0x0C4D, 0x0C8B, 0x0CC9, 0x0D07,
    0x0D45, 0x0D84, 0x0DC2, 0x0E00, 0x0E3F, 0x0E7D, 0x0EBC, 0x0EFA,
    0x0F39, 0x0F78, 0x0FB6, 0x0FF5, 0x1034, 0x1073, 0x10B2, 0x10F1,
    0x1130, 0x116F, 0x11AE, 0x11EE, 0x122D, 0x126C, 0x12AC, 0x12EB,
    0x132B, 0x136B, 0x13AA, 0x13EA, 0x142A, 0x146A, 0x14A9, 0x14E9,
    0x1529, 0x1569, 0x15AA, 0x15EA, 0x162A, 0x166A, 0x16AB, 0x16EB,
    0x172C, 0x176C, 0x17AD, 0x17ED, 0x182E, 0x186F, 0x18B0, 0x18F0,
    0x1931, 0x1972, 0x19B3, 0x19F5, 0x1A36, 0x1A77, 0x1AB8, 0x1AFA,
    0x1B3B, 0x1B7D, 0x1BBE, 0x1C00, 0x1C41, 0x1C83, 0x1CC5, 0x1D07,
    0x1D48, 0x1D8A, 0x1DCC, 0x1E0E, 0x1E51, 0x1E93, 0x1ED5, 0x1F17,
    0x1F5A, 0x1F9C, 0x1FDF, 0x2021, 0x2064, 0x20A6, 0x20E9, 0x212C,
    0x216F, 0x21B2, 0x21F5, 0x2238, 0x227B, 0x22BE, 0x2301, 0x2344,
    0x2388, 0x23CB, 0x240E, 0x2452, 0x2496, 0x24D9, 0x251D, 0x2561,
    0x25A4, 0x25E8, 0x262C, 0x2670, 0x26B4, 0x26F8, 0x273D, 0x2781,
    0x27C5, 0x280A, 0x284E, 0x2892, 0x28D7, 0x291C, 0x2960, 0x29A5,
    0x29EA, 0x2A2F, 0x2A74, 0x2AB9, 0x2AFE, 0x2B43, 0x2B88, 0x2BCD,
    0x2C13, 0x2C58, 0x2C9D, 0x2CE3, 0x2D28, 0x2D6E, 0x2DB4, 0x2DF9,
    0x2E3F, 0x2E85, 0x2ECB, 0x2F11, 0x2F57, 0x2F9D, 0x2FE3, 0x302A,
    0x3070, 0x30B6, 0x30FD, 0x3143, 0x318A, 0x31D0, 0x3217, 0x325E,
    0x32A5, 0x32EC, 0x3332, 0x3379, 0x33C1, 0x3408, 0x344F, 0x3496,
    0x34DD, 0x3525, 0x356C, 0x35B4, 0x35FB, 0x3643, 0x368B, 0x36D3,
    0x371A, 0x3762, 0x37AA, 0x37F2, 0x383A, 0x3883, 0x38CB, 0x3913,
    0x395C, 0x39A4, 0x39ED, 0x3A35, 0x3A7E, 0x3AC6, 0x3B0F, 0x3B58,
    0x3BA1, 0x3BEA, 0x3C33, 0x3C7C, 0x3CC5, 0x3D0E, 0x3D58, 0x3DA1,
    0x3DEA, 0x3E34, 0x3E7D, 0x3EC7, 0x3F11, 0x3F5A, 0x3FA4, 0x3FEE,
    0x4038, 0x4082, 0x40CC, 0x4116, 0x4161, 0x41AB, 0x41F5, 0x4240,
    0x428A, 0x42D5, 0x431F, 0x436A, 0x43B5, 0x4400, 0x444B, 0x4495,
    0x44E1, 0x452C, 0x4577, 0x45C2, 0x460D, 0x4659, 0x46A4, 0x46F0,
    0x473B, 0x4787, 0x47D3, 0x481E, 0x486A, 0x48B6, 0x4902, 0x494E,
    0x499A, 0x49E6, 0x4A33, 0x4A7F, 0x4ACB, 0x4B18, 0x4B64, 0x4BB1,
    0x4BFE, 0x4C4A, 0x4C97, 0x4CE4, 0x4D31, 0x4D7E, 0x4DCB, 0x4E18,
    0x4E66, 0x4EB3, 0x4F00, 0x4F4E, 0x4F9B, 0x4FE9, 0x5036, 0x5084,
    0x50D2, 0x5120, 0x516E, 0x51BC, 0x520A, 0x5258, 0x52A6, 0x52F4,
    0x5343, 0x5391, 0x53E0, 0x542E, 0x547D, 0x54CC, 0x551A, 0x5569,
    0x55B8, 0x5607, 0x5656, 0x56A5, 0x56F4, 0x5744, 0x5793, 0x57E2,
    0x5832, 0x5882, 0x58D1, 0x5921, 0x5971, 0x59C1, 0x5A10, 0x5A60,
    0x5AB0, 0x5B01, 0x5B51, 0x5BA1, 0x5BF1, 0x5C42, 0x5C92, 0x5CE3,
    0x5D34, 0x5D84, 0x5DD5, 0x5E26, 0x5E77, 0x5EC8, 0x5F19, 0x5F6A,
    0x5FBB, 0x600D, 0x605E, 0x60B0, 0x6101, 0x6153, 0x61A4, 0x61F6,
    0x6248, 0x629A, 0x62EC, 0x633E, 0x6390, 0x63E2, 0x6434, 0x6487,
    0x64D9, 0x652C, 0x657E, 0x65D1, 0x6624, 0x6676, 0x66C9, 0x671C,
    0x676F, 0x67C2, 0x6815, 0x6869, 0x68BC, 0x690F, 0x6963, 0x69B6,
    0x6A0A, 0x6A5E, 0x6AB1, 0x6B05, 0x6B59, 0x6BAD, 0x6C01, 0x6C55,
    0x6CAA, 0x6CFE, 0x6D52, 0x6DA7, 0x6DFB, 0x6E50, 0x6EA4, 0x6EF9,
    0x6F4E, 0x6FA3, 0x6FF8, 0x704D, 0x70A2, 0x70F7, 0x714D, 0x71A2,
    0x71F7, 0x724D, 0x72A2, 0x72F8, 0x734E, 0x73A4, 0x73FA, 0x7450,
    0x74A6, 0x74FC, 0x7552, 0x75A8, 0x75FF, 0x7655, 0x76AC, 0x7702,
    0x7759, 0x77B0, 0x7807, 0x785E, 0x78B4, 0x790C, 0x7963, 0x79BA,
    0x7A11, 0x7A69, 0x7AC0, 0x7B18, 0x7B6F, 0x7BC7, 0x7C1F, 0x7C77,
    0x7CCF, 0x7D27, 0x7D7F, 0x7DD7, 0x7E2F, 0x7E88, 0x7EE0, 0x7F38,
    0x7F91, 0x7FEA, 0x8042, 0x809B, 0x80F4, 0x814D, 0x81A6, 0x81FF,
    0x8259, 0x82B2, 0x830B, 0x8365, 0x83BE, 0x8418, 0x8472, 0x84CB,
    0x8525, 0x857F, 0x85D9, 0x8633, 0x868E, 0x86E8, 0x8742, 0x879D,
    0x87F7, 0x8852, 0x88AC, 0x8907, 0x8962, 0x89BD, 0x8A18, 0x8A73,
    0x8ACE, 0x8B2A, 0x8B85, 0x8BE0, 0x8C3C, 0x8C97, 0x8CF3, 0x8D4F,
    0x8DAB, 0x8E07, 0x8E63, 0x8EBF, 0x8F1B, 0x8F77, 0x8FD4, 0x9030,
    0x908C, 0x90E9, 0x9146, 0x91A2, 0x91FF, 0x925C, 0x92B9, 0x9316,
    0x9373, 0x93D1, 0x942E, 0x948C, 0x94E9, 0x9547, 0x95A4, 0x9602,
    0x9660, 0x96BE, 0x971C, 0x977A, 0x97D8, 0x9836, 0x9895, 0x98F3,
    0x9952, 0x99B0, 0x9A0F, 0x9A6E, 0x9ACD, 0x9B2C, 0x9B8B, 0x9BEA,
    0x9C49, 0x9CA8, 0x9D08, 0x9D67, 0x9DC7, 0x9E26, 0x9E86, 0x9EE6,
    0x9F46, 0x9FA6, 0xA006, 0xA066, 0xA0C6, 0xA127, 0xA187, 0xA1E8,
    0xA248, 0xA2A9, 0xA30A, 0xA36B, 0xA3CC, 0xA42D, 0xA48E, 0xA4EF,
    0xA550, 0xA5B2, 0xA613, 0xA675, 0xA6D6, 0xA738, 0xA79A, 0xA7FC,
    0xA85E, 0xA8C0, 0xA922, 0xA984, 0xA9E7, 0xAA49, 0xAAAC, 0xAB0E,
    0xAB71, 0xABD4, 0xAC37, 0xAC9A, 0xACFD, 0xAD60, 0xADC3, 0xAE27,
    0xAE8A, 0xAEED, 0xAF51, 0xAFB5, 0xB019, 0xB07C, 0xB0E0, 0xB145,
    0xB1A9, 0xB20D, 0xB271, 0xB2D6, 0xB33A, 0xB39F, 0xB403, 0xB468,
    0xB4CD, 0xB532, 0xB597, 0xB5FC, 0xB662, 0xB6C7, 0xB72C, 0xB792,
    0xB7F7, 0xB85D, 0xB8C3, 0xB929, 0xB98F, 0xB9F5, 0xBA5B, 0xBAC1,
    0xBB28, 0xBB8E, 0xBBF5, 0xBC5B, 0xBCC2, 0xBD29, 0xBD90, 0xBDF7,
    0xBE5E, 0xBEC5, 0xBF2C, 0xBF94, 0xBFFB, 0xC063, 0xC0CA, 0xC132,
    0xC19A, 0xC202, 0xC26A, 0xC2D2, 0xC33A, 0xC3A2, 0xC40B, 0xC473,
    0xC4DC, 0xC544, 0xC5AD, 0xC616, 0xC67F, 0xC6E8, 0xC751, 0xC7BB,
    0xC824, 0xC88D, 0xC8F7, 0xC960, 0xC9CA, 0xCA34, 0xCA9E, 0xCB08,
    0xCB72, 0xCBDC, 0xCC47, 0xCCB1, 0xCD1B, 0xCD86, 0xCDF1, 0xCE5B,
    0xCEC6, 0xCF31, 0xCF9C, 0xD008, 0xD073, 0xD0DE, 0xD14A, 0xD1B5,
    0xD221, 0xD28D, 0xD2F8, 0xD364, 0xD3D0, 0xD43D, 0xD4A9, 0xD515,
    0xD582, 0xD5EE, 0xD65B, 0xD6C7, 0xD734, 0xD7A1, 0xD80E, 0xD87B,
    0xD8E9, 0xD956, 0xD9C3, 0xDA31, 0xDA9E, 0xDB0C, 0xDB7A, 0xDBE8,
    0xDC56, 0xDCC4, 0xDD32, 0xDDA0, 0xDE0F, 0xDE7D, 0xDEEC, 0xDF5B,
    0xDFC9, 0xE038, 0xE0A7, 0xE116, 0xE186, 0xE1F5, 0xE264, 0xE2D4,
    0xE343, 0xE3B3, 0xE423, 0xE493, 0xE503, 0xE573, 0xE5E3, 0xE654,
    0xE6C4, 0xE735, 0xE7A5, 0xE816, 0xE887, 0xE8F8, 0xE969, 0xE9DA,
    0xEA4B, 0xEABC, 0xEB2E, 0xEB9F, 0xEC11, 0xEC83, 0xECF5, 0xED66,
    0xEDD9, 0xEE4B, 0xEEBD, 0xEF2F, 0xEFA2, 0xF014, 0xF087, 0xF0FA,
    0xF16D, 0xF1E0, 0xF253, 0xF2C6, 0xF339, 0xF3AD, 0xF420, 0xF494,
    0xF507, 0xF57B, 0xF5EF, 0xF663, 0xF6D7, 0xF74C, 0xF7C0, 0xF834,
    0xF8A9, 0xF91E, 0xF992, 0xFA07, 0xFA7C, 0xFAF1, 0xFB66, 0xFBDC,
    0xFC51, 0xFCC7, 0xFD3C, 0xFDB2, 0xFE28, 0xFE9E, 0xFF14, 0xFF8A
};

static uint32_t cmd_queue[64];
static unsigned cmd_queue_count;
static uint32_t shared_mem;
static uint32_t counter;

static SndChannel channels[16];
static SndSequence sequences[16];
static SndTrack tracks[32];
static SndAlarm alarms[8];

static uint8_t chan_volume[16];
static uint8_t chan_pan[16];

static int master_pan;
static int surround_decay;

// 0=regular 1=early/SM64DS
static int version;

static void report_hardware_status(void);
static uint16_t update_counter(void);
static void unlink_channel(SndChannel* chan, bool unlink);


static void reset(void)
{
    cmd_queue_count = 0;
    shared_mem = 0;
    counter = 0;

    memset(channels, 0, sizeof(channels));
    memset(sequences, 0, sizeof(sequences));
    memset(tracks, 0, sizeof(tracks));
    memset(alarms, 0, sizeof(alarms));

    memset(chan_volume, 0, sizeof(chan_volume));
    memset(chan_pan, 0, sizeof(chan_pan));

    master_pan = -1;
    surround_decay = 0;

    for (int i = 0; i < 16; i++)
    {
        SndChannel* chan = &channels[i];

        chan->status_flags &= ~0xF9;
        chan->ID = i;
    }

    for (int i = 0; i < 16; i++)
    {
        SndSequence* seq = &sequences[i];

        seq->status_flags &= ~(1<<0);
        seq->ID = i;
    }

    for (int i = 0; i < 32; i++)
    {
        SndTrack* track = &tracks[i];

        track->status_flags &= ~(1<<0);
    }

    spu_write(2, 0x04000500, 0x807F);


}


static void on_alarm(uint32_t num)
{
    SndAlarm* alarm = &alarms[num];
    if (!alarm->active) return;

    nds_ipc_reply(B, 7u | (num | (alarm->param << 8)) << 6);

    uint32_t delay = alarm->repeat;
    if (delay)
        alarm_set(num, alarm_due[num] + (uint64_t)delay * 64);
    else
    {
        alarm->active = false;
        alarm_set(num, UINT64_MAX);
    }
}


static bool is_channel_playing(int id)
{
    uint32_t addr = 0x04000400 + (id*16);

    uint8_t cnt = spu_read(1, addr+0x03);
    return (cnt & 0x80) != 0;
}

static void stop_channel(int id, bool hold)
{
    uint32_t addr = 0x04000400 + (id*16);

    uint32_t cnt = spu_read(4, addr);
    cnt &= ~(1<<31);
    if (hold) cnt |= (1<<15);
    spu_write(4, addr, cnt);
}

static int calc_channel_volume(int vol, int pan)
{
    if (pan < 24)
    {
        pan += 40;
        pan *= surround_decay;
        pan += ((0x7FFF - surround_decay) << 6);
        return (vol * pan) >> 21;
    }
    else if (pan >= 104)
    {
        pan -= 40;
        pan *= (-surround_decay);
        pan += ((0x7FFF + surround_decay) << 6);
        return (vol * pan) >> 21;
    }

    return vol;
}

static void setup_channel_wave(int id, uint32_t srcaddr, int format, int repeat, uint16_t looppos, uint32_t len, int vol, int voldiv, uint16_t freq, int pan)
{
    uint32_t addr = 0x04000400 + (id*16);

    chan_volume[id] = vol;
    chan_pan[id] = pan;

    if (master_pan >= 0)
        pan = master_pan;

    if ((surround_decay > 0) && (id != 1) && (id != 3))
        vol = calc_channel_volume(vol, pan);

    uint32_t cnt = (format << 29) | (repeat << 27) | (pan << 16) | (voldiv << 8) | vol;
    spu_write(4, addr+0x00, cnt);
    spu_write(2, addr+0x08, 0x10000-freq);
    spu_write(2, addr+0x0A, looppos);
    spu_write(4, addr+0x0C, len);
    spu_write(4, addr+0x04, srcaddr);
}

static void setup_channel_psg(int id, int duty, int vol, int voldiv, uint16_t freq, int pan)
{
    uint32_t addr = 0x04000400 + (id*16);

    chan_volume[id] = vol;
    chan_pan[id] = pan;

    if (master_pan >= 0)
        pan = master_pan;

    if ((surround_decay > 0) && (id != 1) && (id != 3))
        vol = calc_channel_volume(vol, pan);

    uint32_t cnt = (3 << 29) | (duty << 24) | (pan << 16) | (voldiv << 8) | vol;
    spu_write(4, addr+0x00, cnt);
    spu_write(2, addr+0x08, 0x10000-freq);
}

static void setup_channel_noise(int id, int vol, int voldiv, uint16_t freq, int pan)
{
    uint32_t addr = 0x04000400 + (id*16);

    chan_volume[id] = vol;
    chan_pan[id] = pan;

    if (master_pan >= 0)
        pan = master_pan;

    if ((surround_decay > 0) && (id != 1) && (id != 3))
        vol = calc_channel_volume(vol, pan);

    uint32_t cnt = (3 << 29) | (pan << 16) | (voldiv << 8) | vol;
    spu_write(4, addr+0x00, cnt);
    spu_write(2, addr+0x08, 0x10000-freq);
}

static void set_channel_frequency(int id, uint16_t freq)
{
    uint32_t addr = 0x04000400 + (id*16);

    spu_write(2, addr+0x08, 0x10000-freq);
}

static void set_channel_volume(int id, int vol, int voldiv)
{
    uint32_t addr = 0x04000400 + (id*16);

    chan_volume[id] = vol;

    if ((surround_decay > 0) && (id != 1) && (id != 3))
    {
        int pan = spu_read(1, addr+0x02);
        vol = calc_channel_volume(vol, pan);
    }

    uint16_t cnt = (voldiv << 8) | vol;
    spu_write(2, addr+0x00, cnt);
}

static void set_channel_pan(int id, int pan)
{
    uint32_t addr = 0x04000400 + (id*16);

    chan_pan[id] = pan;

    if (master_pan >= 0)
        pan = master_pan;

    spu_write(1, addr+0x02, pan);

    if ((surround_decay > 0) && (id != 1) && (id != 3))
    {
        int vol = calc_channel_volume(chan_volume[id], pan);
        spu_write(1, addr+0x00, vol);
    }
}

static int calc_rate(int rate)
{
    if (rate == 0x7F) return 0xFFFF;
    if (rate == 0x7E) return 0x3C00;
    if (rate < 0x32)
    {
        rate = (rate << 1) + 1;
        return rate & 0xFFFF;
    }

    rate = 0x1E00 / (0x7E - rate);
    return rate & 0xFFFF;
}

static void set_channel_attack_rate(SndChannel* chan, int rate)
{
    if (rate < 109)
    {
        chan->attack_rate = 255 - rate;
    }
    else
    {
        const uint8_t ratetbl[19] =
        {
            0x00, 0x01, 0x05, 0x0E, 0x1A, 0x26, 0x33, 0x3F,
            0x49, 0x54, 0x5C, 0x64, 0x6D, 0x74, 0x7B, 0x7F,
            0x84, 0x89, 0x8F
        };
        rate = 127 - rate;
        chan->attack_rate = ratetbl[rate];
    }
}

static void set_channel_decay_rate(SndChannel* chan, int rate)
{
    chan->decay_rate = calc_rate(rate);
}

static void set_channel_sustain_rate(SndChannel* chan, int rate)
{
    chan->sustain_rate = rate;
}

static void set_channel_release_rate(SndChannel* chan, int rate)
{
    chan->release_rate = calc_rate(rate);
}


static bool is_capture_playing(int id)
{
    uint32_t addr = 0x04000508 + id;

    uint8_t cnt = spu_read(1, addr);
    return (cnt & 0x80) != 0;
}


static void update_hardware_channels(void)
{
    for (int i = 0; i < 16; i++)
    {
        SndChannel* chan = &channels[i];
        if (!(chan->status_flags & 0xF8)) continue;

        if (chan->status_flags & (1<<4))
        {
            stop_channel(i, false);
        }

        if (chan->status_flags & (1<<3))
        {
            if (chan->type == 0)
            {
                setup_channel_wave(i, chan->data_addr, chan->data_format, chan->repeat?1:2, chan->loop_pos, chan->length, chan->volume, chan->volume_div, chan->frequency, chan->pan);
            }
            else if (chan->type == 1)
            {
                setup_channel_psg(i, chan->data_addr, chan->volume, chan->volume_div, chan->frequency, chan->pan);
            }
            else if (chan->type == 2)
            {
                setup_channel_noise(i, chan->volume, chan->volume_div, chan->frequency, chan->pan);
            }

            continue;
        }

        if (chan->status_flags & (1<<5))
        {
            set_channel_frequency(i, chan->frequency);
        }

        if (chan->status_flags & (1<<6))
        {
            set_channel_volume(i, chan->volume, chan->volume_div);
        }

        if (chan->status_flags & (1<<7))
        {
            set_channel_pan(i, chan->pan);
        }
    }

    for (int i = 0; i < 16; i++)
    {
        SndChannel* chan = &channels[i];
        if (!(chan->status_flags & 0xF8)) continue;

        if (chan->status_flags & (1<<3))
        {
            uint32_t addr = 0x04000400 + (i*16) + 0x03;
            spu_write(1, addr, spu_read(1, addr) | 0x80);
        }

        chan->status_flags &= ~0xF8;
    }
}


static void init_track(SndTrack* track)
{
    track->note_buffer = 0;
    track->cur_note_addr = 0;

    track->status_flags |= 0x42;
    track->status_flags &= ~0xBC;

    track->loop_level = 0;
    track->instr_index = 0;
    track->priority = 64;
    track->volume = 127;
    track->expression = 127;
    track->track_unk0_a = 0;
    track->pan = 0;
    track->track_unk09 = 0;
    track->pitch_bend = 0;
    track->frequency = 0;
    track->attack_rate = 255;
    track->decay_rate = 255;
    track->sustain_rate = 255;
    track->release_rate = 255;
    track->track_unk01 = 127;
    track->pitch_bend_range = 2;
    track->track_unk14 = 60;
    track->portamento_time = 0;
    track->sweep_pitch = 0;
    track->transpose = 0;
    track->channel_mask = 0xFFFF;

    track->modulation_type = 0;
    track->modulation_depth = 0;
    track->modulation_range = 1;
    track->modulation_speed = 16;
    track->modulation_delay = 0;

    track->rest_counter = 0;
    track->chan_list = NULL;
}

static void release_track(SndTrack* track, SndSequence* seq, bool flag)
{
    int volbase3 = base_volume_table[seq->volume] + base_volume_table[track->volume] + base_volume_table[track->expression];
    if (volbase3 < -0x8000) volbase3 = -0x8000;

    int volbase1 = track->track_unk0_a + seq->seq_unk06;
    if (volbase1 < -0x8000) volbase1 = -0x8000;

    int freqbase = track->frequency + ((track->pitch_bend * (track->pitch_bend_range << 6)) >> 7);

    int panbase = track->pan;
    if (track->track_unk01 != 0x7F)
        panbase = ((panbase * track->track_unk01) + 64) >> 7;
    panbase += track->track_unk09;
    if (panbase < -0x80) panbase = -0x80;
    else if (panbase > 0x7F) panbase = 0x7F;

    SndChannel* chan = track->chan_list;
    while (chan)
    {
        chan->vol_base1 = volbase1;
        if (chan->vol_ramp_phase != 3)
        {
            chan->vol_base3 = volbase3;
            chan->freq_base3 = freqbase;
            chan->pan_base3 = panbase;
            chan->pan_base1 = track->track_unk01;
            chan->modulation_type = track->modulation_type;
            chan->modulation_speed = track->modulation_speed;
            chan->modulation_depth = track->modulation_depth;
            chan->modulation_range = track->modulation_range;
            chan->modulation_delay = track->modulation_delay;

            if ((chan->note_length == 0) && flag)
            {
                chan->priority = 1;
                chan->vol_ramp_phase = 3;
            }
        }

        chan = chan->next;
    }
}

static void finish_track(SndTrack* track, SndSequence* seq, int rate)
{
    release_track(track, seq, false);

    SndChannel* chan = track->chan_list;
    while (chan)
    {
        if (chan->status_flags & (1<<0))
        {
            if (rate >= 0)
                set_channel_release_rate(chan, rate & 0xFF);

            chan->priority = 1;
            chan->vol_ramp_phase = 3;
        }

        chan = chan->next;
    }
}

static void unlink_track_channels(SndTrack* track)
{
    SndChannel* chan = track->chan_list;
    while (chan)
    {
        chan->linked = false;
        chan->linked_track = NULL;

        chan = chan->next;
    }

    track->chan_list = NULL;
}

static int find_free_track(void)
{
    for (int i = 0; i < 32; i++)
    {
        SndTrack* track = &tracks[i];

        if (!(track->status_flags & (1<<0)))
        {
            track->status_flags |= (1<<0);
            return i;
        }
    }

    return -1;
}

static SndTrack* get_sequence_track(SndSequence* seq, int id)
{
    if (id > 15) return NULL;

    id = seq->tracks[id];
    if (id == 0xFF) return NULL;
    return &tracks[id];
}

static void finish_sequence_track(SndSequence* seq, int id)
{
    SndTrack* track = get_sequence_track(seq, id);
    if (!track) return;

    finish_track(track, seq, -1);
    unlink_track_channels(track);

    track->status_flags &= ~(1<<0);
    seq->tracks[id] = 0xFF;
}

static void init_sequence(SndSequence* seq, uint32_t sbnk)
{
    seq->status_flags &= ~(1<<2);

    seq->sbnk_addr = sbnk;
    seq->tempo = 0x78;
    seq->seq_unk1_a = 0x100;
    seq->tick_counter = 240;
    seq->volume = 127;
    seq->seq_unk06 = 0;
    seq->pan = 64;

    for (int i = 0; i < 16; i++)
        seq->tracks[i] = 0xFF;

    if (shared_mem)
    {
        uint32_t seqdata = shared_mem + (seq->ID * 0x24);

        wr32(seqdata+0x40, 0);
        for (int i = 0; i < 16; i++)
        {
            wr16(seqdata+0x20 + (i<<1), 0xFFFF);
        }
    }
}

static void finish_sequence(SndSequence* seq)
{
    for (int i = 0; i < 16; i++)
        finish_sequence_track(seq, i);

    seq->status_flags &= ~(1<<0);
}


static void prepare_sequence(int id, uint32_t notedata, uint32_t noteoffset, uint32_t sbnk)
{
    SndSequence* seq = &sequences[id];

    if (seq->status_flags & (1<<0))
        finish_sequence(seq);

    init_sequence(seq, sbnk);
    int tnum = find_free_track();
    if (tnum < 0) return;

    SndTrack* track0 = &tracks[tnum];
    init_track(track0);
    track0->note_buffer = notedata;
    track0->cur_note_addr = track0->note_buffer + noteoffset;
    seq->tracks[0] = tnum;

    uint8_t firstcmd = rd8(track0->cur_note_addr);
    if (firstcmd == 0xFE)
    {
        // multi track setup

        track0->cur_note_addr++;

        uint16_t mask = rd8(track0->cur_note_addr++);
        mask |= (rd8(track0->cur_note_addr++) << 8);

        for (int i = 1; i < 16; i++)
        {
            if (!(mask & (1<<i))) continue;

            int tnum2 = find_free_track();
            if (tnum2 < 0) break;

            SndTrack* track = &tracks[tnum2];
            init_track(track);
            seq->tracks[i] = tnum2;
        }
    }

    seq->status_flags |= (1<<0);
    seq->status_flags &= ~(1<<1);

    if (shared_mem)
    {
        uint32_t mask = rd32(shared_mem+4);
        mask |= (1<<id);
        wr32(shared_mem+4, mask);
    }
}

static void start_sequence(int id)
{
    SndSequence* seq = &sequences[id];
    seq->status_flags |= (1<<1);
}

static void process_commands(void)
{
    const uint32_t cmd_trans_early[30] =
    {
        0x0, 0x1, 0x4, 0x6, 0x7, 0x8, 0x9, 0xA,
        0xB, 0xC, 0xD, 0xE, 0xF, 0x10, 0x11, 0x12,
        0x13, 0x14, 0x15, 0x16, 0x17, 0x19, 0x1A, 0x1B,
        0x1C, 0x1D, 0x21, 0x1E, 0x1F, 0x20
    };

    while (cmd_queue_count)
    {
        uint32_t cmdbuf = cmd_queue[0];
        memmove(cmd_queue, cmd_queue + 1, --cmd_queue_count * sizeof *cmd_queue);
        for (;;)
        {
            if (!cmdbuf)
            {
                uint32_t val = rd32(shared_mem);
                val++;
                wr32(shared_mem, val);

                break;
            }

            uint32_t next = rd32(cmdbuf+0x00);
            uint32_t cmd = rd32(cmdbuf+0x04);

            uint32_t args[4];
            for (uint32_t i = 0; i < 4; i++)
                args[i] = rd32(cmdbuf+0x8 + (i*4));

            if (version == 1)
            {
                // COMMAND TRANSLATE for SM64DS (early sound engine version)
                cmd = cmd_trans_early[cmd];
            }
            else if (version == 2)
            {
                if (cmd >= 2)
                    cmd += 3;
            }

            /* a number past the driver's sequences, channels or alarms names none: a library whose commands are
               numbered another way than the version found, which the trace shows */
            if ((cmd <= 0x9 && (args[0] & 0xFFFFFF) >= 16) || (cmd == 0xE && (args[0] & 0xFFFF) >= 16) ||
                ((cmd == 0xF || cmd == 0x10) && args[0] >= 16) || (cmd == 0x12 && args[0] >= 8))
            {
                note("sound cmd %08X out of range, %08X %08X %08X %08X\n", cmd, args[0], args[1], args[2], args[3]);
                cmdbuf = next;
                continue;
            }

            switch (cmd)
            {
            case 0x0: // play sequence, directly
                {
                    prepare_sequence(args[0], args[1], args[2], args[3]);
                    start_sequence(args[0]);
                }
                break;

            case 0x1: // stop sequence
                {
                    uint32_t id = args[0];
                    SndSequence* seq = &sequences[id];
                    if (!(seq->status_flags & (1<<0))) break;

                    finish_sequence(seq);

                    if (shared_mem)
                    {
                        uint32_t mask = rd32(shared_mem+4);
                        mask &= ~(1<<id);
                        wr32(shared_mem+4, mask);
                    }
                }
                break;

            case 0x2: // prepare sequence
                {
                    prepare_sequence(args[0], args[1], args[2], args[3]);
                }
                break;

            case 0x3: // start sequence
                {
                    start_sequence(args[0]);
                }
                break;

            case 0x6: // set sequence param
                {
                    // normally, writes directly to the sequence structure

                    SndSequence* seq = &sequences[args[0]];
                    uint32_t key = (args[3] << 8) | args[1];
                    uint32_t val = args[2];

                    switch (key)
                    {
                    case 0x104: seq->pan = val & 0xFF; break;
                    case 0x105: seq->volume = val & 0xFF; break;

                    case 0x206: seq->seq_unk06 = val & 0xFFFF; break;
                    case 0x218: seq->tempo = val & 0xFFFF; break;
                    case 0x21A: seq->seq_unk1_a = val & 0xFFFF; break;
                    case 0x21C: seq->tick_counter = val & 0xFFFF; break;

                    default:
                        note("cmd6: unknown write %08X %08X\n", key, val);
                        break;
                    }
                }
                break;

            case 0x7: // set track param
                {
                    // normally, writes to the track structure

                    SndSequence* seq = &sequences[args[0] & 0xFFFFFF];
                    uint32_t trackmask = args[1];
                    uint32_t key = ((args[0] >> 24) << 8) | args[2];
                    uint32_t val = args[3];

                    for (int i = 0; i < 16; i++)
                    {
                        if (!(trackmask & (1<<i))) continue;
                        SndTrack* track = get_sequence_track(seq, i);
                        if (!track) continue;

                        switch (key)
                        {
                        case 0x104: track->volume = val & 0xFF; break;
                        case 0x105: track->expression = val & 0xFF; break;
                        case 0x106: track->pitch_bend = val & 0xFF; break;
                        case 0x107: track->pitch_bend_range = val & 0xFF; break;
                        case 0x108: track->pan = val & 0xFF; break;
                        case 0x109: track->track_unk09 = val & 0xFF; break;

                        case 0x20A: track->track_unk0_a = val & 0xFFFF; break;
                        case 0x20C: track->frequency = val & 0xFFFF; break;

                        default:
                            note("cmd7: unknown write %08X %08X\n", key, val);
                            break;
                        }
                    }
                }
                break;

            case 0x9: // set track channel mask
                {
                    SndSequence* seq = &sequences[args[0]];
                    uint32_t trackmask = args[1];
                    uint32_t chanmask = args[2] & 0xFFFF;

                    for (int i = 0; i < 16; i++)
                    {
                        if (!(trackmask & (1<<i))) continue;
                        SndTrack* track = get_sequence_track(seq, i);
                        if (!track) continue;

                        track->channel_mask = chanmask;
                        track->status_flags |= (1<<7);
                    }
                }
                break;

            case 0xA: // write to sharedmem
                {
                    uint32_t addr = shared_mem + (args[0] * 0x24) + (args[1] * 2) + 0x20;
                    wr16(addr, args[2] & 0xFFFF);
                }
                break;

            case 0xC: // start
                {
                    uint32_t mask_chan = args[0];
                    uint32_t mask_cap = args[1];
                    uint32_t mask_alarm = args[2];

                    for (int i = 0; i < 16; i++)
                    {
                        if (!(mask_chan & (1<<i))) continue;

                        uint8_t cnt = spu_read(1, 0x04000403 + (i*16));
                        cnt |= 0x80;
                        spu_write(1, 0x04000403 + (i*16), cnt);
                    }

                    mask_cap &= 0x3;
                    if (mask_cap == 0x3)
                    {
                        uint16_t cnt = spu_read(2, 0x04000508);
                        cnt |= 0x8080;
                        spu_write(2, 0x04000508, cnt);
                    }
                    else if (mask_cap == 0x1)
                    {
                        uint8_t cnt = spu_read(1, 0x04000508);
                        cnt |= 0x80;
                        spu_write(1, 0x04000508, cnt);
                    }
                    else if (mask_cap == 0x2)
                    {
                        uint8_t cnt = spu_read(1, 0x04000509);
                        cnt |= 0x80;
                        spu_write(1, 0x04000509, cnt);
                    }

                    for (int i = 0; i < 8; i++)
                    {
                        if (!(mask_alarm & (1<<i))) continue;

                        SndAlarm* alarm = &alarms[i];
                        alarm->active = true;

                        uint32_t delay = alarm->repeat;
                        if (!delay) delay = alarm->delay;
                        alarm_set((uint32_t)i, B->cycles + (uint64_t)delay * 64);
                    }

                    report_hardware_status();
                }
                break;

            case 0xD: // stop
                {
                    uint32_t mask_chan = args[0];
                    uint32_t mask_cap = args[1];
                    uint32_t mask_alarm = args[2];

                    for (int i = 0; i < 8; i++)
                    {
                        if (!(mask_alarm & (1<<i))) continue;

                        SndAlarm* alarm = &alarms[i];
                        alarm->active = false;

                        alarm_set((uint32_t)i, UINT64_MAX);
                    }

                    for (int i = 0; i < 16; i++)
                    {
                        if (!(mask_chan & (1<<i))) continue;

                        stop_channel(i, args[3]!=0);
                    }

                    if (mask_cap & 0x1)
                    {
                        spu_write(1, 0x04000508, 0);
                    }
                    if (mask_cap & 0x2)
                    {
                        spu_write(1, 0x04000509, 0);
                    }

                    report_hardware_status();
                }
                break;

            case 0xE: // setup channel (wave)
                {
                    int id = args[0] & 0xFFFF;
                    uint32_t srcaddr = args[1] & 0x07FFFFFF;
                    int format = (args[3] >> 24) & 0x3;
                    int repeat = (args[3] >> 26) & 0x3;
                    uint16_t looppos = args[3] & 0xFFFF;
                    uint32_t len = args[2] & 0x3FFFFF;
                    int vol = (args[2] >> 24) & 0x7F;
                    int voldiv = (args[2] >> 22) & 0x3;
                    uint16_t freq = args[0] >> 16;
                    int pan = (args[3] >> 16) & 0x7F;

                    setup_channel_wave(id, srcaddr, format, repeat, looppos, len, vol, voldiv, freq, pan);
                }
                break;

            case 0xF: // setup channel (PSG)
                {
                    int id = args[0];
                    int duty = args[3];
                    int vol = args[1] & 0x7F;
                    int voldiv = (args[1] >> 8) & 0x3;
                    uint16_t freq = (args[2] >> 8) & 0xFFFF;
                    int pan = args[2] & 0x7F;

                    setup_channel_psg(id, duty, vol, voldiv, freq, pan);
                }
                break;

            case 0x10: // setup channel (noise)
                {
                    int id = args[0];
                    int vol = args[1] & 0x7F;
                    int voldiv = (args[1] >> 8) & 0x3;
                    uint16_t freq = (args[2] >> 8) & 0xFFFF;
                    int pan = args[2] & 0x7F;

                    setup_channel_noise(id, vol, voldiv, freq, pan);
                }
                break;

            case 0x11: // setup capture
                {
                    uint32_t dstaddr = args[0];
                    uint16_t len = args[1] & 0xFFFF;

                    uint32_t num = (args[2] >> 31) & 0x1;

                    uint32_t cnt = ((args[2] >> 30) & 0x1) << 3;
                    cnt |= (((args[2] >> 29) & 0x1) ? 0 : 0x04);
                    cnt |= ((args[2] >> 28) & 0x1) << 1;
                    cnt |= ((args[2] >> 27) & 0x1);

                    spu_write(1, 0x04000508+num, cnt);
                    spu_write(4, 0x04000510+(num*8), dstaddr);
                    spu_write(2, 0x04000514+(num*8), len);
                }
                break;

            case 0x12: // setup alarm
                {
                    uint32_t num = args[0];

                    SndAlarm* alarm = &alarms[num & 0x7];
                    alarm->delay = args[1];
                    alarm->repeat = args[2];
                    alarm->param = args[3] & 0xFF;
                    alarm->active = false; // checkme
                }
                break;

            case 0x14: // set channel volume
                {
                    for (int i = 0; i < 16; i++)
                    {
                        if (!(args[0] & (1<<i))) continue;

                        set_channel_volume(i, args[1], args[2]);
                    }
                }
                break;

            case 0x15: // set channel pan
                {
                    for (int i = 0; i < 16; i++)
                    {
                        if (!(args[0] & (1<<i))) continue;

                        set_channel_pan(i, args[1]);
                    }
                }
                break;

            case 0x16: // set surround decay
                {
                    surround_decay = args[0];

                    for (int i = 0; i < 16; i++)
                    {
                        if (i == 1 || i == 3) continue;

                        uint32_t chanaddr = 0x04000400 + (i*16);
                        uint8_t pan = spu_read(1, chanaddr+0x02);
                        int vol = calc_channel_volume(chan_volume[i], pan);
                        spu_write(1, chanaddr+0x00, vol);
                    }
                }
                break;

            case 0x17: // set master volume
                {
                    spu_write(1, 0x04000500, args[0] & 0xFF);
                }
                break;

            case 0x18: // set master pan
                {
                    master_pan = args[0];
                    if (master_pan >= 0)
                    {
                        uint8_t pan = master_pan & 0xFF;
                        for (int i = 0; i < 16; i++)
                        {
                            spu_write(1, 0x04000402 + (i*16), pan);
                        }
                    }
                    else
                    {
                        for (int i = 0; i < 16; i++)
                        {
                            spu_write(1, 0x04000402 + (i*16), chan_pan[i]);
                        }
                    }
                }
                break;

            case 0x19: // output sel
                {
                    uint32_t outputL = args[0];
                    uint32_t outputR = args[1];
                    uint32_t mixch1 = args[2];
                    uint32_t mixch3 = args[3];

                    uint8_t cnt = rd8(0x04000501);
                    cnt &= 0x80;
                    cnt |= (outputL & 0x3);
                    cnt |= (outputR & 0x3) << 2;
                    cnt |= (mixch1 & 0x1) << 4;
                    cnt |= (mixch3 & 0x1) << 5;
                    wr8(0x04000501, cnt);
                }
                break;

            case 0x1A:
                {
                    //
                    note("unknown sound cmd %08X, %08X %08X %08X %08X\n",
                       cmd, args[0], args[1], args[2], args[3]);
                }
                break;

            case 0x1D:
                shared_mem = args[0];
                break;

            case 0x20:
                {
                    // cmdbuf+08 and cmdbuf+0C = bounds
                    // stop channels conditionally
                    note("unknown sound cmd %08X, %08X %08X %08X %08X\n",
                       cmd, args[0], args[1], args[2], args[3]);
                }
                break;

            default:
                note("unknown sound cmd %08X, %08X %08X %08X %08X\n",
                       cmd, args[0], args[1], args[2], args[3]);
                break;
            }

            cmdbuf = next;
        }
    }
}


static bool read_instrument(uint32_t sbnk, int index, int tune, uint8_t* out)
{
    if (index < 0) return false;

    uint32_t numinstr = rd32(sbnk + 0x38);
    if ((uint32_t)index >= numinstr) return false;

    uint32_t val = rd32(sbnk + 0x3C + (index << 2));
    out[0] = val & 0xFF;
    if (out[0] >= 1 && out[0] <= 5)
    {
        uint32_t addr = sbnk + (val >> 8);
        for (int i = 0; i < 5; i++)
        {
            *(uint16_t*)&out[2 + (i << 1)] = rd16(addr + (i << 1));
        }
        return true;
    }
    else if (out[0] == 16)
    {
        uint32_t addr = sbnk + (val >> 8);
        uint8_t lower = rd8(addr);
        uint8_t upper = rd8(addr + 1);

        if (tune < lower) return false;
        if (tune > upper) return false;

        addr += ((tune - lower) * 0xC) + 2;
        for (int i = 0; i < 6; i++)
        {
            *(uint16_t*)&out[(i << 1)] = rd16(addr + (i << 1));
        }
        return true;
    }
    else if (out[0] == 17)
    {
        uint32_t addr = sbnk + (val >> 8);

        int num = -1;
        for (int i = 0; i < 8; i++)
        {
            uint8_t val = rd8(addr + i);
            if (tune > val) continue;

            num = i;
            break;
        }

        if (num < 0) return false;

        addr += (num * 0xC) + 8;
        for (int i = 0; i < 6; i++)
        {
            *(uint16_t*)&out[(i << 1)] = rd16(addr + (i << 1));
        }
        return true;
    }

    return false;
}

static void init_instrument_channel(SndChannel* chan, int len)
{
    chan->base_volume = -92544;
    chan->vol_ramp_phase = 0;
    chan->note_length = len;
    chan->modulation_count1 = 0;
    chan->modulation_count2 = 0;
    chan->status_flags |= 0x03;
}

static bool setup_instrument(SndChannel* chan, int tune, int speed, int len, uint32_t sbnk, uint8_t* data)
{
    uint8_t release = data[0x0A];
    if (release == 0xFF)
    {
        release = 0;
        len = -1;
    }

    switch (data[0x00])
    {
    case 1: // wave
    case 4:
        {
            uint32_t swav;
            if (data[0x00] != 1)
            {
                // direct pointer

                swav = (*(uint16_t*)&data[0x02]) | ((*(uint16_t*)&data[0x04]) << 16);
            }
            else
            {
                // load from SBNK

                uint16_t swav_num = *(uint16_t*)&data[0x02];
                uint16_t swar_num = *(uint16_t*)&data[0x04];

                uint32_t swar = rd32(sbnk + 0x18 + (swar_num << 3));
                if (!swar) return false;

                uint32_t num_samples = rd32(swar + 0x38);
                if (swav_num >= num_samples) return false;

                swav = rd32(swar + 0x3C + (swav_num << 2));
                if (!swav) return false;
                if (swav < 0x02000000)
                    swav += swar;
            }

            if (!swav) return false;

            chan->type = 0; // wave
            chan->data_format = rd8(swav+0x00);
            chan->repeat = rd8(swav+0x01);
            chan->sample_rate = rd16(swav+0x02);
            chan->swav_frequency = rd16(swav+0x04);
            chan->loop_pos = rd16(swav+0x06);
            chan->length = rd32(swav+0x08);
            chan->data_addr = swav+0xC;
        }
        break;

    case 2: // PSG
        {
            uint16_t duty = *(uint16_t*)&data[0x02];

            if ((chan->ID < 8) || (chan->ID > 13))
                return false;

            chan->type = 1; // PSG
            chan->data_addr = duty;
            chan->swav_frequency = 0x1F46;
        }
        break;

    case 3: // noise
        {
            if ((chan->ID < 14) || (chan->ID > 15))
                return false;

            chan->type = 2; // noise
            chan->swav_frequency = 0x1F46;
        }
        break;

    default:
        return false;
    }

    init_instrument_channel(chan, len);
    chan->freq_base2 = tune;
    chan->freq_base1 = *(uint16_t*)&data[0x06]; // note number
    chan->vol_base2 = speed;
    set_channel_attack_rate(chan, data[0x07]);
    set_channel_decay_rate(chan, data[0x08]);
    set_channel_sustain_rate(chan, data[0x09]);
    set_channel_release_rate(chan, release);
    chan->pan_base2 = data[0x0B] - 64;
    return true;
}

static SndChannel* allocate_channel(uint16_t chanmask, int prio, bool flag, SndTrack* track)
{
    (void)flag;
    const int chanorder[16] = {4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13};
    const int voldiv[4] = {0, 1, 2, 4};
    SndChannel* ret = NULL;

    // TODO: channel lock masks
    chanmask &= 0xFFF5;

    for (int i = 0; i < 16; i++)
    {
        int id = chanorder[i];
        if (!(chanmask & (1 << id))) continue;

        SndChannel* chan = &channels[id];
        if (!ret)
        {
            ret = chan;
            continue;
        }

        if (chan->priority > ret->priority)
            continue;

        if (chan->priority == ret->priority)
        {
            uint32_t vol1 = (chan->volume << 4) >> voldiv[chan->volume_div];
            uint32_t vol2 = (ret->volume << 4) >> voldiv[ret->volume_div];

            if (vol1 >= vol2)
                continue;
        }

        ret = chan;
    }

    if (!ret) return NULL;
    if (prio < ret->priority) return NULL;

    if (ret->linked)
        unlink_channel(ret, false);

    ret->status_flags &= ~0xF9;
    ret->status_flags |= (1<<4);

    ret->next = NULL;
    ret->linked = true;
    ret->linked_track = track;
    ret->note_length = 0;
    ret->priority = prio;
    ret->volume = 127;
    ret->volume_div = 0;
    ret->status_flags &= ~(1<<1);
    ret->status_flags |= (1<<2);
    ret->freq_base2 = 60;
    ret->freq_base1 = 60;
    ret->vol_base2 = 127;
    ret->pan_base2 = 0;
    ret->vol_base3 = 0;
    ret->vol_base1 = 0;
    ret->freq_base3 = 0;
    ret->pan_base3 = 0;
    ret->pan_base1 = 127;
    ret->freq_ramp_target = 0;
    ret->freq_ramp_len = 0;
    ret->freq_ramp_pos = 0;
    set_channel_attack_rate(ret, 127);
    set_channel_decay_rate(ret, 127);
    set_channel_sustain_rate(ret, 127);
    set_channel_release_rate(ret, 127);
    ret->modulation_type = 0;
    ret->modulation_depth = 0;
    ret->modulation_range = 1;
    ret->modulation_speed = 16;
    ret->modulation_delay = 0;

    return ret;
}

static void track_key_on(SndTrack* track, SndSequence* seq, int tune, int speed, int len)
{
    //note("sound_nitro: KEY ON, seq=%d, tune=%d speed=%d len=%d\n", seq->ID, tune, speed, len);

    SndChannel* chan = NULL;
    if (track->status_flags & (1<<3))
    {
        chan = track->chan_list;
        if (chan)
        {
            chan->freq_base2 = tune;
            chan->vol_base2 = speed;
        }
    }

    if (!chan)
    {
        // allocate channel

        uint8_t instrdata[16];
        if (!read_instrument(seq->sbnk_addr, track->instr_index, tune, instrdata))
            return;

        uint16_t chanmask;
        switch (instrdata[0])
        {
        case 1:
        case 4: chanmask = 0xFFFF; break;
        case 2: chanmask = 0x3F00; break;
        case 3: chanmask = 0xC000; break;
        default: return;
        }

        chanmask &= track->channel_mask;
        chan = allocate_channel(chanmask, track->priority, (track->status_flags & (1<<7)) != 0, track);
        if (!chan) return;

        int _len;
        if (track->status_flags & (1<<3))
            _len = -1;
        else
            _len = len;

        if (!setup_instrument(chan, tune, speed, _len, seq->sbnk_addr, instrdata))
        {
            chan->priority = 0;
            chan->linked = false;
            chan->linked_track = NULL;
            return;
        }

        chan->next = track->chan_list;
        track->chan_list = chan;
    }

    if (track->attack_rate != 0xFF)
        set_channel_attack_rate(chan, track->attack_rate);
    if (track->decay_rate != 0xFF)
        set_channel_decay_rate(chan, track->decay_rate);
    if (track->sustain_rate != 0xFF)
        set_channel_sustain_rate(chan, track->sustain_rate);
    if (track->release_rate != 0xFF)
        set_channel_release_rate(chan, track->release_rate);

    chan->freq_ramp_target = track->sweep_pitch;
    if (track->status_flags & (1<<5))
    {
        chan->freq_ramp_target += (((track->track_unk14 - tune) << 22) >> 16);
    }

    if (!track->portamento_time)
    {
        chan->freq_ramp_len = len;
        chan->status_flags &= ~(1<<2);
    }
    else
    {
        int time = track->portamento_time;
        time *= time;

        int target = chan->freq_ramp_target;
        if (target < 0) target = -target;
        time *= target;
        chan->freq_ramp_len = time >> 11;
    }
    chan->freq_ramp_pos = 0;
}

static uint32_t get_note_param_addr(SndSequence* seq, uint8_t idx)
{
    if (!shared_mem) return 0;

    if (idx >= 0x10)
    {
        return shared_mem + 0x260 + ((idx-0x10) << 1);
    }

    return shared_mem + 0x20 + (seq->ID * 0x24) + (idx << 1);
}

static uint32_t read_note_op_param(SndTrack* track, SndSequence* seq, int type)
{
    switch (type)
    {
    case 0: // 8-bit value
        {
            uint8_t val = rd8(track->cur_note_addr++);
            return val;
        }

    case 1: // 16-bit value
        {
            uint32_t val = rd8(track->cur_note_addr++);
            val |= (rd8(track->cur_note_addr++) << 8);
            return val;
        }

    case 2: // variable length
        {
            uint32_t val = 0;
            for (;;)
            {
                uint8_t byte = rd8(track->cur_note_addr++);
                val = (val << 7) | (byte & 0x7F);
                if (!(byte & 0x80)) break;
            }
            return val;
        }

    case 3: // based on counter
        {
            int16_t val1 = rd8(track->cur_note_addr++);
            val1 |= (rd8(track->cur_note_addr++) << 8);
            int16_t val2 = rd8(track->cur_note_addr++);
            val2 |= (rd8(track->cur_note_addr++) << 8);

            uint16_t cnt = update_counter();
            int res = ((val2 - val1) + 1) * cnt;
            res = val1 + (res >> 16);
            return res;
        }

    case 4: // variable in sharedmem
        {
            uint8_t idx = rd8(track->cur_note_addr++);
            uint32_t addr = get_note_param_addr(seq, idx);
            if (addr)
            {
                uint32_t val = rd16(addr);
                return ((int32_t)(val << 16)) >> 16;
            }

            return 0;
        }
    }

    return 0;
}

static int update_track(SndTrack* track, SndSequence* seq, int id, bool process)
{
    (void)id;
    SndChannel* chan = track->chan_list;
    while (chan)
    {
        if (chan->note_length > 0)
            chan->note_length--;

        if (!(chan->status_flags & (1<<2)))
        {
            if (chan->freq_ramp_pos < chan->freq_ramp_len)
                chan->freq_ramp_pos++;
        }

        chan = chan->next;
    }

    if (track->status_flags & (1<<4))
    {
        if (track->chan_list) return 0;
        track->status_flags &= ~(1<<4);
    }

    if (track->rest_counter > 0)
    {
        track->rest_counter--;
        if (track->rest_counter > 0)
            return 0;
    }

    while (!track->rest_counter)
    {
        if (track->status_flags & (1<<4)) break;

        bool cond = true;
        int paramtype = 2;

        uint8_t note_op = rd8(track->cur_note_addr++);
        if (note_op == 0xA2)
        {
            note_op = rd8(track->cur_note_addr++);
            cond = (track->status_flags & (1<<6)) != 0;
        }
        if (note_op == 0xA0)
        {
            note_op = rd8(track->cur_note_addr++);
            paramtype = 3;
        }
        if (note_op == 0xA1)
        {
            note_op = rd8(track->cur_note_addr++);
            paramtype = 4;
        }

        if (!(note_op & 0x80))
        {
            // key on

            uint8_t speed = rd8(track->cur_note_addr++);
            int32_t len = read_note_op_param(track, seq, paramtype);
            int tune = note_op + track->transpose;
            if (!cond) continue;

            if (tune < 0) tune = 0;
            else if (tune > 127) tune = 127;

            if ((!(track->status_flags & (1<<2))) && process)
            {
                track_key_on(track, seq, tune, speed, (len<=0) ? -1 : len);
            }

            track->track_unk14 = tune;
            if (track->status_flags & (1<<1))
            {
                track->rest_counter = len;
                if (!len) track->status_flags |= (1<<4);
            }
        }
        else
        {
            switch (note_op & 0xF0)
            {
            case 0x80:
                {
                    uint32_t param = read_note_op_param(track, seq, paramtype);
                    if (!cond) break;

                    switch (note_op)
                    {
                    case 0x80: // rest
                        track->rest_counter = param;
                        break;

                    case 0x81: // bank/program number
                        if ((int32_t)param < 0x10000)
                            track->instr_index = param;
                        break;

                    default:
                        note("sound_nitro: unknown note op %02X\n", note_op);
                        break;
                    }
                }
                break;

            case 0x90:
                {
                    switch (note_op)
                    {
                    case 0x93: // setup track
                        {
                            uint8_t tnum = rd8(track->cur_note_addr++);
                            uint32_t trackaddr = rd8(track->cur_note_addr++);
                            trackaddr |= (rd8(track->cur_note_addr++) << 8);
                            trackaddr |= (rd8(track->cur_note_addr++) << 16);
                            if (!cond) break;

                            SndTrack* thetrack = get_sequence_track(seq, tnum);
                            if (!thetrack) break;
                            if (thetrack == track) break;

                            finish_track(thetrack, seq, -1);
                            unlink_track_channels(thetrack);

                            thetrack->note_buffer = track->note_buffer;
                            thetrack->cur_note_addr = thetrack->note_buffer + trackaddr;
                        }
                        break;

                    case 0x94: // jump
                        {
                            uint32_t jumpaddr = rd8(track->cur_note_addr++);
                            jumpaddr |= (rd8(track->cur_note_addr++) << 8);
                            jumpaddr |= (rd8(track->cur_note_addr++) << 16);
                            if (!cond) break;

                            track->cur_note_addr = track->note_buffer + jumpaddr;
                        }
                        break;

                    case 0x95: // call
                        {
                            uint32_t jumpaddr = rd8(track->cur_note_addr++);
                            jumpaddr |= (rd8(track->cur_note_addr++) << 8);
                            jumpaddr |= (rd8(track->cur_note_addr++) << 16);
                            if (!cond) break;

                            if (track->loop_level >= 3) break;
                            track->loop_addr[track->loop_level] = track->cur_note_addr;
                            track->loop_level++;
                            track->cur_note_addr = track->note_buffer + jumpaddr;
                        }
                        break;

                    default:
                        note("sound_nitro: unknown note op %02X\n", note_op);
                        break;
                    }
                }
                break;

            case 0xB0:
                {
                    uint8_t idx = rd8(track->cur_note_addr++);
                    if (paramtype == 2) paramtype = 1;
                    int16_t param = (int16_t)((int32_t)(read_note_op_param(track, seq, paramtype) << 16) >> 16);
                    uint32_t paramaddr = get_note_param_addr(seq, idx);
                    if (!cond) break;
                    if (!paramaddr) break;

                    switch (note_op)
                    {
                    case 0xB0: // set
                        wr16(paramaddr, param);
                        break;
                    case 0xB1: // add
                        wr16(paramaddr, (int16_t)rd16(paramaddr) + param);
                        break;
                    case 0xB2: // sub
                        wr16(paramaddr, (int16_t)rd16(paramaddr) - param);
                        break;
                    case 0xB3: // mul
                        wr16(paramaddr, (int16_t)rd16(paramaddr) * param);
                        break;
                    case 0xB4: // div
                        if (!param) break;
                        wr16(paramaddr, (int16_t)rd16(paramaddr) / param);
                        break;
                    case 0xB5: // shift
                        if (param >= 0)
                            wr16(paramaddr, (int16_t)rd16(paramaddr) << param);
                        else
                            wr16(paramaddr, (int16_t)rd16(paramaddr) >> (-param));
                        break;
                    case 0xB6: //
                        {
                            bool neg = false;
                            if (param < 0)
                            {
                                neg = true;
                                param = -param;
                            }

                            uint16_t cnt = update_counter();
                            int val = ((int)cnt * (param + 1)) >> 16;
                            if (neg) val = -val;
                            wr16(paramaddr, val);
                        }
                        break;
                    case 0xB8: // compare ==
                        track->status_flags &= ~(1<<6);
                        if ((int16_t)rd16(paramaddr) == param)
                            track->status_flags |= (1<<6);
                        break;
                    case 0xB9: // compare >=
                        track->status_flags &= ~(1<<6);
                        if ((int16_t)rd16(paramaddr) >= param)
                            track->status_flags |= (1<<6);
                        break;
                    case 0xBA: // compare >
                        track->status_flags &= ~(1<<6);
                        if ((int16_t)rd16(paramaddr) > param)
                            track->status_flags |= (1<<6);
                        break;
                    case 0xBB: // compare <=
                        track->status_flags &= ~(1<<6);
                        if ((int16_t)rd16(paramaddr) <= param)
                            track->status_flags |= (1<<6);
                        break;
                    case 0xBC: // compare <
                        track->status_flags &= ~(1<<6);
                        if ((int16_t)rd16(paramaddr) < param)
                            track->status_flags |= (1<<6);
                        break;
                    case 0xBD: // compare !=
                        track->status_flags &= ~(1<<6);
                        if ((int16_t)rd16(paramaddr) != param)
                            track->status_flags |= (1<<6);
                        break;

                    default:
                        note("sound_nitro: unknown note op %02X\n", note_op);
                        break;
                    }
                }
                break;

            case 0xC0:
            case 0xD0:
                {
                    if (paramtype == 2) paramtype = 0;
                    uint8_t param = (uint8_t)read_note_op_param(track, seq, paramtype);
                    if (!cond) break;

                    switch (note_op)
                    {
                    case 0xC0: // pan
                        track->pan = param - 64;
                        break;
                    case 0xC1: // volume
                        track->volume = param;
                        break;
                    case 0xC2: // master volume
                        seq->volume = param;
                        break;
                    case 0xC3: // transpose
                        track->transpose = param;
                        break;
                    case 0xC4: // pitch bend
                        track->pitch_bend = param;
                        break;
                    case 0xC5: // pitch bend range
                        track->pitch_bend_range = param;
                        break;
                    case 0xC6: // priority
                        track->priority = param;
                        break;
                    case 0xC7: // mono/poly
                        track->status_flags &= ~(1<<1);
                        track->status_flags |= ((param & 0x1) << 1);
                        break;
                    case 0xC8: // tie
                        track->status_flags &= ~(1<<3);
                        track->status_flags |= ((param & 0x1) << 3);
                        finish_track(track, seq, -1);
                        unlink_track_channels(track);
                        break;
                    case 0xC9: // portamento control
                        track->track_unk14 = param + track->transpose;
                        track->status_flags |= (1<<5);
                        if (version == 1)
                        {
                            finish_track(track, seq, -1);
                            unlink_track_channels(track);
                        }
                        break;
                    case 0xCA: // modulation depth
                        track->modulation_depth = param;
                        break;
                    case 0xCB: // modulation speed
                        track->modulation_speed = param;
                        break;
                    case 0xCC: // modulation type
                        track->modulation_type = param;
                        break;
                    case 0xCD: // modulation range
                        track->modulation_range = param;
                        break;
                    case 0xCE: // portamento on/off
                        track->status_flags &= ~(1<<5);
                        track->status_flags |= ((param & 0x1) << 5);
                        if (version == 1)
                        {
                            finish_track(track, seq, -1);
                            unlink_track_channels(track);
                        }
                        break;
                    case 0xCF: // portamento time
                        track->portamento_time = param;
                        break;
                    case 0xD0: // attack rate
                        track->attack_rate = param;
                        break;
                    case 0xD1: // decay rate
                        track->decay_rate = param;
                        break;
                    case 0xD2: // sustain rate
                        track->sustain_rate = param;
                        break;
                    case 0xD3: // release rate
                        track->release_rate = param;
                        break;
                    case 0xD4: // loop start marker
                        if (track->loop_level >= 3) break;
                        track->loop_addr[track->loop_level] = track->cur_note_addr;
                        track->loop_count[track->loop_level] = param;
                        track->loop_level++;
                        break;
                    case 0xD5: // expression
                        track->expression = param;
                        break;

                    default:
                        note("sound_nitro: unknown note op %02X\n", note_op);
                        break;
                    }
                }
                break;

            case 0xE0:
                {
                    if (paramtype == 2) paramtype = 1;
                    int16_t param = (int16_t)((int32_t)(read_note_op_param(track, seq, paramtype) << 16) >> 16);
                    if (!cond) break;

                    switch (note_op)
                    {
                    case 0xE0: // modulation delay
                        track->modulation_delay = param;
                        break;
                    case 0xE1: // tempo
                        seq->tempo = param;
                        break;
                    case 0xE3: // sweep pitch
                        track->sweep_pitch = param;
                        break;

                    default:
                        note("sound_nitro: unknown note op %02X\n", note_op);
                        break;
                    }
                }
                break;

            case 0xF0:
                {
                    if (!cond) break;
                    switch (note_op)
                    {
                    case 0xFC: // loop end marker
                        {
                            if (track->loop_level == 0) break;

                            int level = track->loop_level - 1;
                            uint8_t cnt = track->loop_count[level];
                            if (cnt != 0)
                            {
                                cnt--;
                                if (!cnt)
                                {
                                    track->loop_level--;
                                    break;
                                }
                            }

                            track->loop_count[level] = cnt;
                            track->cur_note_addr = track->loop_addr[level];
                        }
                        break;

                    case 0xFD: // return from call
                        {
                            if (track->loop_level == 0) break;

                            int level = track->loop_level - 1;
                            track->cur_note_addr = track->loop_addr[level];
                            track->loop_level--;
                        }
                        break;

                    case 0xFE: // multitrack marker (handled at sequence setup)
                        break;
                    case 0xFF: // track end
                        return -1;

                    default:
                        note("sound_nitro: unknown note op %02X\n", note_op);
                        break;
                    }
                }
                break;

            default:
                note("sound_nitro: unknown note op %02X\n", note_op);
                break;
            }
        }
    }

    return 0;
}

static bool update_sequence_tracks(SndSequence* seq, bool process)
{
    bool ret = true;

    for (int i = 0; i < 16; i++)
    {
        SndTrack* track = get_sequence_track(seq, i);
        if (!track) continue;
        if (!track->cur_note_addr) continue;

        if (!update_track(track, seq, i, process))
        {
            ret = false;
            continue;
        }

        finish_sequence_track(seq, i);
    }

    return ret;
}

static void update_sequence(SndSequence* seq)
{
    int cnt = 0;
    while (seq->tick_counter >= 240)
    {
        seq->tick_counter -= 240;
        cnt++;
    }

    int i;
    for (i = 0; i < cnt; i++)
    {
        if (update_sequence_tracks(seq, true))
        {
            finish_sequence(seq);
            break;
        }
    }

    if (shared_mem)
    {
        uint32_t addr = shared_mem + 0x40 + (seq->ID * 0x24);
        wr32(addr, rd32(addr) + i);
    }

    seq->tick_counter += ((seq->tempo * seq->seq_unk1_a) >> 8);
}

static void process_sequences(bool update)
{
    uint16_t activemask = 0;

    for (int i = 0; i < 16; i++)
    {
        SndSequence* seq = &sequences[i];
        if (!(seq->status_flags & (1<<0))) continue;

        if (seq->status_flags & (1<<1))
        {
            if (update && (!(seq->status_flags & (1<<2))))
                update_sequence(seq);

            for (int j = 0; j < 16; j++)
            {
                SndTrack* track = get_sequence_track(seq, j);
                if (!track) continue;

                release_track(track, seq, true);
            }
        }

        if (seq->status_flags & (1<<0))
            activemask |= (1<<i);
    }

    if (shared_mem)
        wr32(shared_mem+4, activemask);
}


static void unlink_channel(SndChannel* chan, bool unlink)
{
    SndTrack* track = chan->linked_track;

    if (unlink)
    {
        chan->priority = 0;
        chan->linked = false;
        chan->linked_track = NULL;
    }

    if (track->chan_list == chan)
    {
        track->chan_list = chan->next;
        return;
    }

    SndChannel* chan2 = track->chan_list;
    do
    {
        if (chan2->next == chan)
        {
            chan2->next = chan->next;
            return;
        }

        chan2 = chan2->next;
    }
    while (chan2);
}

static int channel_volume_ramp(SndChannel* chan, bool update)
{
    if (update)
    {
        if (chan->vol_ramp_phase == 0)
        {
            // attack

            chan->base_volume = -(((-chan->base_volume) * chan->attack_rate) >> 8);
            if (chan->base_volume == 0)
                chan->vol_ramp_phase = 1;
        }
        else if (chan->vol_ramp_phase == 1)
        {
            // decay

            chan->base_volume -= chan->decay_rate;
            int target = base_volume_table[chan->sustain_rate & 0x7F] << 7;
            if (chan->base_volume <= target)
            {
                chan->base_volume = target;
                chan->vol_ramp_phase = 2;
            }
        }
        else if (chan->vol_ramp_phase == 3)
        {
            // release

            chan->base_volume -= chan->release_rate;
        }
    }

    return chan->base_volume >> 7;
}

static int channel_freq_ramp(SndChannel* chan, bool update)
{
    if (chan->freq_ramp_target == 0)
        return 0;

    if (chan->freq_ramp_pos >= chan->freq_ramp_len)
        return 0;

    int64_t tmp = (int64_t)chan->freq_ramp_target * (int64_t)(chan->freq_ramp_len-chan->freq_ramp_pos);
    int ret = (int)(tmp / (int64_t)chan->freq_ramp_len);

    if (update)
    {
        if (chan->status_flags & (1<<2))
            chan->freq_ramp_pos++;
    }

    return ret;
}

static int channel_modulation(SndChannel* chan, bool update)
{
    int64_t modfactor = 0;
    if ((chan->modulation_depth != 0) &&
        (chan->modulation_count1 >= chan->modulation_delay))
    {
        int index = chan->modulation_count2 >> 8;
        if (index < 32)
            modfactor = modulation_table[index];
        else if (index < 64)
            modfactor = modulation_table[64 - index];
        else if (index < 96)
            modfactor = -modulation_table[index - 64];
        else
            modfactor = -modulation_table[32 - (index - 96)];

        modfactor *= chan->modulation_depth;
        modfactor *= chan->modulation_range;
    }

    if (modfactor)
    {
        switch (chan->modulation_type)
        {
        case 0:
            modfactor <<= 6;
            break;
        case 1:
            modfactor *= 60;
            break;
        case 2:
            modfactor <<= 6;
            break;
        }

        modfactor >>= 14;
    }

    if (update)
    {
        if (chan->modulation_count1 < chan->modulation_delay)
        {
            chan->modulation_count1++;
        }
        else
        {
            uint32_t cnt = (chan->modulation_count2 + (chan->modulation_speed << 6)) >> 8;
            while (cnt >= 128) cnt -= 128;

            chan->modulation_count2 += (chan->modulation_speed << 6);
            chan->modulation_count2 &= 0x00FF;
            chan->modulation_count2 |= (cnt << 8);
        }
    }

    return (int)modfactor;
}

static uint16_t calc_volume(int vol)
{
    if (vol < -723) vol = -723;
    else if (vol > 0) vol = 0;

    uint8_t ret = swi_volume_table[vol + 723];

    int div = 0;
    if (vol < -240) div = 3;
    else if (vol < -120) div = 2;
    else if (vol < -60) div = 1;

    return (div << 8) | ret;
}

static uint16_t calc_freq(uint32_t unk, int freq)
{
    freq = -freq;

    int div = 0;
    while (freq < 0)
    {
        div--;
        freq += 768;
    }
    while (freq >= 768)
    {
        div++;
        freq -= 768;
    }

    uint64_t pitch = swi_pitch_table[freq] + 0x10000;
    pitch *= unk;

    div -= 16;
    if (div <= 0)
    {
        pitch >>= (-div);
    }
    else if (div < 32)
    {
        pitch <<= div;
        // CHECKME
        if (!pitch) return 0xFFFF;
    }
    else
    {
        return 0xFFFF;
    }

    if (pitch < 0x10)
        pitch = 0x10;
    else if (pitch > 0xFFFF)
        pitch = 0xFFFF;

    return (uint16_t)(pitch & 0xFFFF);
}

static void update_channels(bool updateramps)
{
    for (int i = 0; i < 16; i++)
    {
        SndChannel* chan = &channels[i];
        if (!(chan->status_flags & (1<<0))) continue;

        if (chan->status_flags & (1<<1))
        {
            chan->status_flags |= (1<<3);
            chan->status_flags &= ~(1<<1);
        }
        else if (!is_channel_playing(i))
        {
            if (chan->linked)
                unlink_channel(chan, true);
            else
                chan->priority = 0;

            chan->volume = 0;
            chan->volume_div = 0;
            chan->status_flags &= ~(1<<0);
            continue;
        }

        int vol = base_volume_table[chan->vol_base2 & 0x7F];
        int freq = (chan->freq_base2 - chan->freq_base1) << 6;
        int pan = 0;

        vol += channel_volume_ramp(chan, updateramps);
        freq += channel_freq_ramp(chan, updateramps);

        vol += chan->vol_base3;
        vol += chan->vol_base1;
        freq += chan->freq_base3;

        int mod = channel_modulation(chan, updateramps);
        switch (chan->modulation_type)
        {
        case 0:
            freq += mod;
            break;
        case 1:
            if (vol > -0x8000)
                vol += mod;
            break;
        case 2:
            pan += mod;
            break;
        }

        if (chan->pan_base1 != 0x7F)
        {
            pan = ((pan * (int)chan->pan_base1) + 64) >> 7;
        }
        pan += chan->pan_base3;

        if ((chan->vol_ramp_phase == 3) && (vol <= -0x2D3))
        {
            chan->status_flags &= ~0xF8;
            chan->status_flags |= (1<<4);

            if (chan->linked)
                unlink_channel(chan, true);
            else
                chan->priority = 0;

            chan->volume = 0;
            chan->volume_div = 0;
            chan->status_flags &= ~(1<<0);
            continue;
        }

        uint16_t finalvol = calc_volume(vol);

        uint16_t finalfreq = calc_freq(chan->swav_frequency, freq);
        if (chan->type == 1) finalfreq &= 0xFFFC;

        pan += 64;
        if (pan < 0) pan = 0;
        else if (pan > 127) pan = 127;
        uint8_t finalpan = (uint8_t)pan;

        if (finalvol != (chan->volume | (chan->volume_div<<8)))
        {
            chan->volume = finalvol & 0xFF;
            chan->volume_div = finalvol >> 8;
            chan->status_flags |= (1<<6);
        }

        if (finalfreq != chan->frequency)
        {
            chan->frequency = finalfreq;
            chan->status_flags |= (1<<5);
        }

        if (finalpan != chan->pan)
        {
            chan->pan = finalpan;
            chan->status_flags |= (1<<7);
        }
    }
}


static void report_hardware_status(void)
{
    if (!shared_mem) return;

    uint16_t chanmask = 0;
    for (int i = 0; i < 16; i++)
    {
        if (is_channel_playing(i))
            chanmask |= (1<<i);
    }

    uint16_t capmask = 0;
    if (is_capture_playing(0)) capmask |= (1<<0);
    if (is_capture_playing(1)) capmask |= (1<<1);

    wr16(shared_mem+0x08, chanmask);
    wr16(shared_mem+0x0A, capmask);
}


static uint16_t update_counter(void)
{
    counter = (counter * 0x19660D) + 0x3C6EF35F;
    return (counter >> 16);
}


static void process(uint32_t param)
{
    update_hardware_channels();
    process_commands();
    process_sequences(param!=0);
    update_channels(param!=0);
    report_hardware_status();
    update_counter();
}


/* ---- the board's side ---- */

bool nds_sound_init(NdsBoard* board) {
    board->sound = calloc(1, sizeof *board->sound);
    if (!board->sound || !nds_spu_init(board)) return false;
    B = board;
    for (unsigned i = 0; i < 8; ++i) alarm_due[i] = UINT64_MAX;
    reset();
    /* SDK 2.0's sound library numbers its commands in two other ways: the early one of its first builds (melonDS has
       the table, cmd_trans_early, and there a sequence ends a note at a portamento's change), and the one of its
       later builds, three numbers short from command 2 on. melonDS knows them by game code. The program's SDK
       version says the same: 2.0 with a build step below the release candidates' is the early one */
    version = 0;
    if (board->rom->sdk_version && (board->rom->sdk_version >> 16) <= 0x0200)
        version = (board->rom->sdk_version & 0xFFFF) < 0x4000 ? 1 : 2;
    return true;
}

void nds_sound_free(NdsBoard* board) {
    nds_spu_free(board);
    free(board->sound);
    board->sound = NULL;
}

void nds_sound_request(NdsBoard* board, uint32_t data) {
    B = board;
    if (data == 0)
        process(0); /* the command lists sent so far, now */
    else if (data >= 0x02000000u && cmd_queue_count < 64)
        cmd_queue[cmd_queue_count++] = data;
}

void nds_sound_tick(NdsBoard* board) {
    B = board;
    process(1);
    nds_spu_run(board);
}

void nds_sound_alarm(NdsBoard* board) {
    B = board;
    for (uint32_t i = 0; i < 8; ++i)
        if (alarm_due[i] <= board->cycles) on_alarm(i);
    alarm_set(0, alarm_due[0]);
}
