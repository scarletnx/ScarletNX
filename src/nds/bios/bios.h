/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NDS_BIOS_H
#define SNX_NDS_BIOS_H
#include "nds/board/nds.h"
/* The ARM9 BIOS's software interrupts as board routines: the console's BIOS is not part of the emulator and none is
   read. The numbers and what each takes and gives are GBATEK's "BIOS Functions". */
enum {
    NDS_SWI_SOFT_RESET = 0x00,
    NDS_SWI_WAIT_BY_LOOP = 0x03,
    NDS_SWI_INTR_WAIT = 0x04,
    NDS_SWI_VBLANK_INTR_WAIT = 0x05,
    NDS_SWI_HALT = 0x06,
    NDS_SWI_DIV = 0x09,
    NDS_SWI_CPU_SET = 0x0B,
    NDS_SWI_CPU_FAST_SET = 0x0C,
    NDS_SWI_SQRT = 0x0D,
    NDS_SWI_GET_CRC16 = 0x0E,
    NDS_SWI_IS_DEBUGGER = 0x0F,
    NDS_SWI_BIT_UNPACK = 0x10,
    NDS_SWI_LZ77_WRAM = 0x11,
    NDS_SWI_LZ77_CALLBACK = 0x12,
    NDS_SWI_HUFF_CALLBACK = 0x13,
    NDS_SWI_RL_WRAM = 0x14,
    NDS_SWI_RL_CALLBACK = 0x15,
    NDS_SWI_DIFF8 = 0x16,
    NDS_SWI_DIFF16 = 0x18,
    NDS_SWI_CUSTOM_POST = 0x1F
};
/* Where the machine is while it is in the BIOS, which a game's interrupt handler sees and may keep in a thread's
   context: the return of an interrupt's entry, the loop IntrWait halts in, the halt of Halt, and the return of a
   software interrupt. No code is there: the machine loop does what the BIOS's would (cpu/cpu.c) */
#define NDS_BIOS_IRQ_RETURN 0xFFFF0290u
#define NDS_BIOS_INTR_WAIT  0xFFFF02D0u
#define NDS_BIOS_HALT       0xFFFF02F0u
#define NDS_BIOS_SWI_RETURN 0xFFFF0310u
/* the BIOS's CRC-16 (the polynomial 0xA001), which the firmware's settings are summed with too */
uint16_t nds_crc16(uint16_t crc, const unsigned char* bytes, uint32_t length);
/* A call that ends at once, on the registers r: 0, or -1 for a number the BIOS does not have. cycles gets the bus
   cycles it takes more than a few (WaitByLoop's) */
int nds_bios_call(NdsBoard* board, uint32_t number, uint32_t r[16], uint64_t* cycles);
#endif
