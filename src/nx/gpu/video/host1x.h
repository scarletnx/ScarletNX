/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* host1x (host1x.c): the NVDEC and VIC engines behind nvdrv's /dev/nvhost-nvdec and /dev/nvhost-vic */
#ifndef SNX_HOST1X_H
#define SNX_HOST1X_H
#include "common/cpu/aarch64/cpu.h"

/* A channel: what one open fd of either device owns (method registers, syncpoints, job queue and, for NVDEC, a
   decoder). Channels only meet through guest memory and syncpoints */
typedef struct H1Ch H1Ch;
H1Ch* host1x_open(int is_vic); /* NULL when out of memory */
/* The fd closed: its queued jobs run first, then the decoder and the syncpoints go. NULL is ignored */
void host1x_close(H1Ch* ch);
/* An ioctl on the channel's fd, with its buffer d of n bytes. Returns the NvError */
u32 host1x_ioctl(H1Ch* ch, u32 cmd, u8* d, u32 n);
/* a game pause: 0 once every channel's queued jobs have run, -1 when some are left after timeout_ms */
int host1x_drain(int timeout_ms);
/* nvmap freed handle h: whatever the engines mapped of it is unmapped, its IOVA range free again */
void host1x_forget_handle(u32 h);
/* Save states. The IOVA table, which the game holds addresses of (MAP_BUFFER's): written into io, and read back in
   place of the running one. 0, or -1 */
struct StateIo;
typedef struct H1Iova H1Iova;
int host1x_iova_save(struct StateIo* io);
H1Iova* host1x_iova_parse(struct StateIo* io); /* read into a copy, NULL when damaged */
void host1x_iova_install(H1Iova* t);           /* in place of the running table, the copy freed */
void host1x_iova_free(H1Iova* t);
/* A channel's state (its syncpoints, the stream's class and method, the engine's registers), and a channel opened
   with it (NULL when io holds no channel or memory ran out). Its jobs ran before the save (the game was paused,
   host1x_drain). An NVDEC channel's decoder starts again: a movie loaded in its middle shows what its next pictures
   decode to without their references until its next key frame */
int host1x_channel_save(const H1Ch* ch, struct StateIo* io);
H1Ch* host1x_channel_load(struct StateIo* io);
/* a channel host1x_channel_load made for a load that was refused: closed without freeing the syncpoints it names,
   which are the running game's ids too */
void host1x_channel_discard(H1Ch* ch);
/* Routes FFmpeg's log (warnings and worse, a limited number) into the game log. nvdec.c defines it; every user of
   libavcodec calls it before opening a decoder */
void ffmpeg_log_init(void);

#endif
