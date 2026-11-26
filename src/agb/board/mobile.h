/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Mobile Adapter GB on the link port, through libmobile (REONTeam's, LGPL-3.0-or-later): REON runs the adapter's
   service again for the games that used it */
#ifndef SNX_AGB_MOBILE_H
#define SNX_AGB_MOBILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AgbMobile;
/* The adapter, its configuration (MOBILE_CONFIG_SIZE bytes: the game's and libmobile's) kept in the file config, made
   at the first write, and dns, when not NULL, the DNS server it asks when a game gives none. NULL with why when it
   cannot be made */
struct AgbMobile* agb_mobile_open(const char* config, const uint8_t dns[4], char* why, size_t size);
void agb_mobile_close(struct AgbMobile* m);
/* A transfer of the serial port's Normal mode the game clocked, its 8 or 32 bits sent: what the adapter sent back */
uint32_t agb_mobile_exchange(struct AgbMobile* m, uint32_t sent, bool word);
/* the adapter's own work (its connections, its timeouts): at least every 100 ms */
void agb_mobile_loop(struct AgbMobile* m);
/* trace: each line the library says, on stderr */
void agb_mobile_trace(struct AgbMobile* m, bool on);
#endif
