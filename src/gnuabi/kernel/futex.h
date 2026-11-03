/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_FUTEX_H
#define SNX_GNU_FUTEX_H
#include "gnuabi/kernel/kernel.h"
void gnu_futex_init(GnuFutexTable* table);
/* futex(2): WAIT, WAKE, REQUEUE, CMP_REQUEUE, WAKE_OP, WAIT_BITSET and WAKE_BITSET, PRIVATE and CLOCK_REALTIME */
int64_t gnu_futex(GnuKernelThread* thread, const uint64_t arguments[6]);
/* FUTEX_WAKE of count waiters on the word at address, for the kernel (a thread's clear_tid): how many woke */
int gnu_futex_wake(GnuKernel* kernel, uint64_t address, int count);
/* a fork's child: the other threads, and their waits, are gone */
void gnu_futex_forked(GnuFutexTable* table);
#endif
