/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory's writes, seen (watch.c): the pages the renderer copied to the GPU, write-protected, so it copies them
   again only once they were written. A write the runtime makes itself (the GPU's, recorded) is told; one by the
   game's code or the runtime's C code faults once, which marks the page written and lets the write through. A
   generation counts the writes: a copy keeps the one it was made at, and its memory changed when a page of it was
   written after. When tracking is unavailable, every range is treated as changed. */
#ifndef SNX_VITA_WATCH_H
#define SNX_VITA_WATCH_H
#include <stdbool.h>
#include <stdint.h>

struct VitaProcess;
bool vita_watch_init(struct VitaProcess* p);
void vita_watch_free(void);
/* [address, address + size) write-protected: the generation its contents are now, for vita_watch_changed */
uint64_t vita_watch_protect(uint32_t address, uint32_t size);
/* whether a page of [address, address + size) was written since generation since, or isn't watched */
bool vita_watch_changed(uint32_t address, uint32_t size, uint64_t since);
/* a write the runtime records into the range (a GPU store): its pages written, their protection kept */
void vita_watch_written(uint32_t address, uint32_t size);
/* the range writable again and written: before the system writes it (a file read), and when it is committed or
   decommitted */
void vita_watch_release(uint32_t address, uint32_t size);
/* a write fault at host address: true when it was a watched page's, now writable again (from the signal handler) */
bool vita_watch_fault(uintptr_t host);
#endif
