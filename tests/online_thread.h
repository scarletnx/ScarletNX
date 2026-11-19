/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* One thread beside a test's own, for a server the test talks to */
#ifndef SNX_ONLINE_TEST_THREAD_H
#define SNX_ONLINE_TEST_THREAD_H
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN /* winsock2.h's sockets, not winsock.h's, for the tests that include both */
#endif
#include <windows.h>
typedef HANDLE SnxOnlineThread;
typedef DWORD(WINAPI* SnxOnlineThreadMain)(void*);
#define SNX_ONLINE_THREAD_MAIN(name, arg) static DWORD WINAPI name(void* arg)
#define SNX_ONLINE_THREAD_END             return 0
static int snx_online_thread_start(SnxOnlineThread* t, SnxOnlineThreadMain f, void* arg) {
    *t = CreateThread(NULL, 0, f, arg, 0, NULL);
    return *t ? 0 : -1;
}
static void snx_online_thread_join(SnxOnlineThread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#include <pthread.h>
typedef pthread_t SnxOnlineThread;
typedef void* (*SnxOnlineThreadMain)(void*);
#define SNX_ONLINE_THREAD_MAIN(name, arg) static void* name(void* arg)
#define SNX_ONLINE_THREAD_END             return NULL
static int snx_online_thread_start(SnxOnlineThread* t, SnxOnlineThreadMain f, void* arg) {
    return pthread_create(t, NULL, f, arg);
}
static void snx_online_thread_join(SnxOnlineThread t) { pthread_join(t, NULL); }
#endif
#endif
