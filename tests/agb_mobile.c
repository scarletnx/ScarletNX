/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Mobile Adapter GB on the link port (src/agb/board/mobile.c): a game's Begin Session packet clocked out a byte at
   a time in the serial port's Normal 8 bit mode, as the console's master side sends it, the adapter's acknowledgment
   in the bytes it clocks back, then its answer to the packet, NINTENDO again, after the idle bytes the game sends while
   it waits; the board with no adapter still reads 0. No network: Begin Session reaches no server */
#include "agb/board/board.h"
#include "agb/board/mobile.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

/* one byte clocked out on the internal clock, as a game sends it: the byte the cable gave back */
static uint8_t clock_byte(AgbBoard* board, uint8_t out) {
    agb_write16(board, 0x04000128, 0x0001); /* normal 8 bit mode, the internal clock, the start bit clear */
    agb_write16(board, 0x0400012A, out);
    agb_write16(board, 0x04000128, 0x0081); /* start */
    agb_sio_step(board, 1u << 16);
    return (uint8_t)agb_read16(board, 0x0400012A);
}

int main(void) {
    AgbBoard board;
    char* dir = snx_temp_dir(NULL);
    char* config = dir ? snx_path_join(dir, "mobile_adapter.bin") : NULL;
    char why[256] = "";
    CHECK(config != NULL);
    agb_board_init(&board);
    agb_write16(&board, 0x04000134, 0x0000); /* RCNT: the serial modes */
    CHECK(clock_byte(&board, 0x99) == 0);    /* no adapter: no partner's byte */
    board.mobile = agb_mobile_open(config, NULL, why, sizeof why);
    if (!board.mobile) {
        fprintf(stderr, "agb_mobile: no adapter: %s\n", why);
        return 1;
    }
    /* Begin Session: 99 66, command 10, 00, length 0008, "NINTENDO", the checksum of the command, the length and the
       data, then the GBA's device byte (81) and a byte for the adapter's answer */
    static const uint8_t data[8] = {'N', 'I', 'N', 'T', 'E', 'N', 'D', 'O'};
    uint8_t packet[32] = {0x99, 0x66, 0x10, 0x00, 0x00, 0x08};
    unsigned n = 6, sum = 0x10 + 0x08;
    for (unsigned i = 0; i < 8; ++i) packet[n++] = data[i], sum += data[i];
    packet[n++] = (uint8_t)(sum >> 8), packet[n++] = (uint8_t)sum;
    packet[n++] = 0x81;
    packet[n++] = 0x00;
    uint8_t back[1024];
    unsigned got = 0;
    for (unsigned i = 0; i < n; ++i) back[got++] = clock_byte(&board, packet[i]);
    /* the adapter's acknowledgment in the packet's last bytes: its device byte (88 to 8B) as the console sends its
       own, then the command xor 80 */
    int device = 0, acked = 0;
    for (unsigned i = n - 4; i < got; ++i) {
        device |= (back[i] & 0xFC) == 0x88;
        acked |= device && back[i] == 0x90;
    }
    CHECK(device && acked);
    /* the answer: the game sends idle bytes (4B) while the adapter's loop runs, and its packet comes back */
    for (unsigned round = 0; round < 200 && got < sizeof back; ++round) {
        agb_mobile_loop(board.mobile);
        back[got++] = clock_byte(&board, 0x4B);
    }
    int answered = 0;
    for (unsigned i = n; i + 14 < got && !answered; ++i)
        answered = back[i] == 0x99 && back[i + 1] == 0x66 && back[i + 2] == 0x90 && back[i + 4] == 0 &&
                   back[i + 5] == 8 && !memcmp(back + i + 6, data, 8);
    if (!answered) {
        fprintf(stderr, "agb_mobile: the adapter's bytes after Begin Session:");
        for (unsigned i = 0; i < got && i < 80; ++i) fprintf(stderr, " %02X", back[i]);
        fputc('\n', stderr);
    }
    CHECK(answered);
    agb_board_destroy(&board); /* the adapter goes with the board */
    (void)snx_remove_tree(dir);
    free(config);
    free(dir);
    if (!failures) puts("agb_mobile: Begin Session answered through the link port");
    return failures ? 1 : 0;
}
