/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A member of an Eden room (Eden's src/network/room.cpp and room_member.cpp, the protocol yuzu and Citra had): a
   server that relays, over one ENet connection per member, ldn's messages and the games' datagrams between the
   consoles in it, so they play local wireless across the internet. The room gives each member an address of its own
   (192.168.x.y), which is the console's for as long as it is in the room: ldn's nodes carry it, and the game's UDP
   sockets (snx_room_socket_open) send from it and are reached at it.

   The wire is the room's: the message type in the first byte, then big-endian integers, strings and byte vectors
   as a u32 length and the bytes, IPv4 addresses as their 4 bytes, every message reliable on channel 0. A datagram's
   bytes travel as a zstd frame.

   One thread per member runs ENet (enet_host_service every 5 ms, as Eden's member does) and calls the ldn handler
   and fills the sockets from there; the calls below may come from any thread. */
#ifndef SNX_NET_ROOM_H
#define SNX_NET_ROOM_H
#include "common/network/net.h"

enum {
    SNX_ROOM_PORT = 24872,         /* the room's default port */
    SNX_ROOM_VERSION = 1,          /* network_version: a room of another version refuses the join */
    SNX_ROOM_NICKNAME_MAX = 20,    /* the room's rule (room.cpp's IsValidNickname) */
    SNX_ROOM_DATAGRAM_MAX = 65507, /* what an IPv4 UDP datagram holds */
    SNX_ROOM_JOIN_MS = 5000        /* to connect, then again for the room's answer: Eden's ConnectionTimeoutMs */
};

/* why a join failed or the room went away; SNX_ROOM_IN while in it */
enum {
    SNX_ROOM_IN = 0,
    SNX_ROOM_NO_ADDRESS,       /* the host's name has no address */
    SNX_ROOM_LOCAL,            /* this computer couldn't make the connection (out of memory, no socket) */
    SNX_ROOM_UNREACHABLE,      /* no ENet connection within SNX_ROOM_JOIN_MS */
    SNX_ROOM_NO_ANSWER,        /* connected, and no answer to the join within SNX_ROOM_JOIN_MS */
    SNX_ROOM_NAME_COLLISION,   /* IdNameCollision: another member has the nickname, or the room's rule refuses it */
    SNX_ROOM_IP_COLLISION,     /* IdIpCollision */
    SNX_ROOM_VERSION_MISMATCH, /* IdVersionMismatch */
    SNX_ROOM_WRONG_PASSWORD,   /* IdWrongPassword */
    SNX_ROOM_FULL,             /* IdRoomIsFull */
    SNX_ROOM_BANNED,           /* IdHostBanned */
    SNX_ROOM_KICKED,           /* IdHostKicked */
    SNX_ROOM_CLOSED            /* IdCloseRoom, or the connection to the room dropped */
};
/* what each SNX_ROOM_* means, for the logs and the error that stops the game */
const char* snx_room_why_text(int why);

typedef struct SnxRoom SnxRoom;

/* Joins the room at host (a name or a dotted address) and port as nickname, with password ("" for none), and tells
   it the game (name, title id, version: what the room shows of each member). Blocks up to twice SNX_ROOM_JOIN_MS.
   0 and the member, or -SNX_E* with why (SNX_ROOM_*) */
int snx_room_join(SnxRoom** out, const char* host, uint16_t port, const char* nickname, const char* password,
                  const char* game_name, uint64_t game_id, const char* game_version, int* why);
/* leaves the room. The member stays until its last user lets it go too: each socket it opened and each snx_room_ref
   holds it, and a socket out of the room fails its sends with SNX_ENETDOWN */
void snx_room_leave(SnxRoom* r);
void snx_room_ref(SnxRoom* r);
void snx_room_unref(SnxRoom* r);
/* the console's address in the room */
void snx_room_fake_ip(const SnxRoom* r, uint8_t ip[4]);
/* SNX_ROOM_IN while in the room, else why it went away */
int snx_room_state(SnxRoom* r);
/* Local-wireless games need prompt delivery. Opt into a 1 ms service interval
   and periodic relay transport diagnostics; other room users retain 5 ms. */
void snx_room_low_latency(SnxRoom* r);
/* 1 while a member of the room has address ip (this console included) */
int snx_room_has_member(SnxRoom* r, const uint8_t ip[4]);

/* ldn's messages (Eden's LDNPacketType); local_ip is the sender's address in the room, remote_ip the receiver's
   (ignored for a broadcast, which reaches every other member). data is a NodeInfo or a NetworkInfo, or nothing */
enum {
    SNX_ROOM_LDN_SCAN = 0,
    SNX_ROOM_LDN_SCAN_RESPONSE = 1,
    SNX_ROOM_LDN_CONNECT = 2,
    SNX_ROOM_LDN_SYNC_NETWORK = 3,
    SNX_ROOM_LDN_DISCONNECT = 4,
    SNX_ROOM_LDN_DESTROY_NETWORK = 5
};
typedef struct SnxRoomLdn {
    uint8_t type;
    uint8_t local_ip[4], remote_ip[4];
    int broadcast;
    const uint8_t* data;
    size_t size;
} SnxRoomLdn;
/* handler(arg, packet) is called from the room's thread for every ldn message, and with packet NULL when the members
   changed (one came or left) or the room went away. One handler per member: set_ldn is -SNX_EBUSY while another arg
   has it; clear_ldn takes arg's off (another's stays) once a call under way has ended, so arg may be freed then */
int snx_room_set_ldn(SnxRoom* r, void (*handler)(void* arg, const SnxRoomLdn* packet), void* arg);
void snx_room_clear_ldn(SnxRoom* r, void* arg);
/* sends an ldn message from this console's address; 0, -SNX_ENETDOWN out of the room, -SNX_EMSGSIZE, -SNX_ENOMEM */
int snx_room_send_ldn(SnxRoom* r, uint8_t type, const uint8_t remote_ip[4], int broadcast, const void* data,
                      size_t size);

/* A datagram socket in the room (SNX_AF_INET, SNX_SOCK_DGRAM, UDP or 0), polled and timed out as any other. It
   binds 0.0.0.0 or the console's address (an unbound one takes a free port on its first send), sends to the
   members' addresses (a broadcast, with SO_BROADCAST, to any address ending in 255) and receives what is sent to its
   port, a broadcast only with SO_BROADCAST, from the sender's address and port. -SNX_EPROTONOSUPPORT for anything
   else */
int snx_room_socket_open(SnxRoom* r, SnxSocket** out, int domain, int type, int protocol);
int snx_room_socket(const SnxSocket* s); /* 1 for a room's socket */

/* The room's messages, for the tests: the join request, an ldn message and a datagram (its data as given, the
   zstd frame included), each the bytes the room gets, or 0 when they don't fit max */
size_t snx_room_pack_join(uint8_t* out, size_t max, const char* nickname, const char* password);
size_t snx_room_pack_ldn(uint8_t* out, size_t max, const SnxRoomLdn* packet);
typedef struct SnxRoomDatagram {
    uint8_t local_ip[4], remote_ip[4]; /* the sender's and the receiver's */
    uint16_t local_port, remote_port;  /* host order */
    int broadcast;
    const uint8_t* data;
    size_t size;
} SnxRoomDatagram;
size_t snx_room_pack_datagram(uint8_t* out, size_t max, const SnxRoomDatagram* d);
/* the inverse of the last two: 1 when in is that message, whole, with data pointing into in */
int snx_room_parse_ldn(const uint8_t* in, size_t size, SnxRoomLdn* out);
int snx_room_parse_datagram(const uint8_t* in, size_t size, SnxRoomDatagram* out);
#endif
