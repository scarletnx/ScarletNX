/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Eden room for the tests (fake_room.c): an ENet server on 127.0.0.1 that reads every message a member sends and
   relays them as Eden's src/network/room.cpp does, from its own reading of the bytes, so a field the member puts in the
   wrong place fails here, and says who joins, leaves or is kicked as Eden's does. Four members, password "secret", the
   room's nickname rule. A failure aborts the test */
#ifndef SNX_TEST_FAKE_ROOM_H
#define SNX_TEST_FAKE_ROOM_H
#include <stddef.h>
#include <stdint.h>

typedef struct FakeRoom FakeRoom;
FakeRoom* fake_room_start(void);
void fake_room_stop(FakeRoom* f);
uint16_t fake_room_port(const FakeRoom* f);
void fake_room_datagram_delay(FakeRoom* f, unsigned milliseconds);
void fake_room_close(FakeRoom* f);                      /* IdCloseRoom to every member */
void fake_room_kick(FakeRoom* f, const char* nickname); /* IdHostKicked to it, then its connection closed */
/* IdChatMessage with text to every member but the one named nickname, as if it had sent text */
void fake_room_chat(FakeRoom* f, const char* nickname, const char* text);
/* how many ldn messages of type the room relayed, and the size of the last one's data */
int fake_room_ldn_count(FakeRoom* f, int type, size_t* last_size);
/* the start of the last datagram message relayed: its size, up to max bytes copied */
size_t fake_room_last_datagram(FakeRoom* f, uint8_t* out, size_t max);
/* the game the last IdSetGameInfo named */
void fake_room_last_game(FakeRoom* f, char* name, size_t size, uint64_t* id);
#endif
