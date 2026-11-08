/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_HID_H
#define SNX_CTR_HID_H
#include <stdint.h>
struct SnxCtrProcess;
/* hid:USER's samples due by now_ns written to its shared memory, each with its event signalled: the pad's every 4.27
   ms from the first GetIPCHandles, the accelerometer's and the gyroscope's while a game has them enabled */
void snx_ctr_hid_update(struct SnxCtrProcess* p, uint64_t now_ns);
/* the host monotonic ns of the next sample, UINT64_MAX for none */
uint64_t snx_ctr_hid_deadline(const struct SnxCtrProcess* p);
/* A stick's direction bits as the circle pad's (bits 28 to 31: right, left, up, down; ir:rst's C-stick has them four
   places down), from its raw position x and y: none near the center (Azahar's GetStickDirectionState) */
uint32_t snx_ctr_hid_stick_directions(int32_t x, int32_t y);
#endif
