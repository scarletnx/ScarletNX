/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* syscall */
#endif
#include "common/memory/host.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#endif

#ifdef _WIN32
/* Windows 10 1803's placeholder calls, looked up (an older Windows has none, and no views) */
static struct {
    PVOID(WINAPI* alloc2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    PVOID(WINAPI* map3)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    BOOL(WINAPI* unmap2)(HANDLE, PVOID, ULONG);
    INIT_ONCE looked;
} g_host = {.looked = INIT_ONCE_STATIC_INIT};

/* the first caller looks the calls up and any other waits for it: a thread must never see the lookup begun and its
   result not yet stored, which would read as a host with no views */
static BOOL CALLBACK look(PINIT_ONCE once, PVOID parameter, PVOID* context) {
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    (void)once;
    (void)parameter;
    (void)context;
    if (k) {
        *(FARPROC*)&g_host.alloc2 = GetProcAddress(k, "VirtualAlloc2");
        *(FARPROC*)&g_host.map3 = GetProcAddress(k, "MapViewOfFile3");
        *(FARPROC*)&g_host.unmap2 = GetProcAddress(k, "UnmapViewOfFile2");
    }
    return TRUE;
}
static bool calls(void) {
    InitOnceExecuteOnce(&g_host.looked, look, NULL, NULL);
    return g_host.alloc2 && g_host.map3 && g_host.unmap2;
}
static DWORD protection(unsigned permissions) {
    return permissions & SNX_HOST_WRITE ? PAGE_READWRITE : permissions ? PAGE_READONLY : PAGE_NOACCESS;
}
size_t snx_host_page(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return calls() ? info.dwPageSize : 0;
}
bool snx_host_memory_new(SnxHostMemory* m, uint64_t size) {
    *m = (SnxHostMemory){0};
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, (DWORD)(size >> 32),
                                        (DWORD)size, NULL);
    if (!section) return false;
    m->view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size);
    if (!m->view) {
        CloseHandle(section);
        return false;
    }
    m->handle = (intptr_t)section;
    m->size = size;
    return true;
}
void snx_host_memory_free(SnxHostMemory* m) {
    if (m->view) UnmapViewOfFile(m->view);
    if (m->handle) CloseHandle((HANDLE)m->handle);
    *m = (SnxHostMemory){0};
}
unsigned char* snx_host_reserve(uint64_t size) {
    if (!calls()) return NULL;
    return g_host.alloc2(GetCurrentProcess(), NULL, (SIZE_T)size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                         NULL, 0);
}
void snx_host_release(unsigned char* base, uint64_t size) {
    if (!calls()) return;
    /* a view, a commit and a placeholder are each an allocation of their own: each goes alone */
    for (uint64_t at = 0; at < size;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(base + at, &info, sizeof info) || !info.RegionSize) break;
        if (info.Type == MEM_MAPPED)
            g_host.unmap2(GetCurrentProcess(), base + at, 0);
        else if (info.State != MEM_FREE)
            VirtualFree(base + at, 0, MEM_RELEASE);
        at += info.RegionSize;
    }
}
/* Each page is a placeholder of its own when it is first mapped (split out of the reservation's), and a view of the
   object's page while it is mapped: no range ever spans two views, which Windows would refuse to protect or unmap
   whole */
bool snx_host_map(const SnxHostMemory* m, unsigned char* base, uint64_t at, uint64_t offset, uint64_t size,
                  unsigned permissions) {
    size_t page = snx_host_page();
    for (uint64_t done = 0; done < size; done += page) {
        unsigned char* p = base + at + done;
        VirtualFree(p, page, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER); /* fails once it is split: harmless */
        if (!g_host.map3((HANDLE)m->handle, GetCurrentProcess(), p, offset + done, page, MEM_REPLACE_PLACEHOLDER,
                         protection(permissions), NULL, 0)) {
            if (done) snx_host_unmap(base, at, done);
            return false;
        }
    }
    return true;
}
bool snx_host_commit(unsigned char* base, uint64_t at, uint64_t size) {
    if (!calls()) return false;
    VirtualFree(base + at, (SIZE_T)size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER); /* split out: fails when it is one */
    return g_host.alloc2(GetCurrentProcess(), base + at, (SIZE_T)size,
                         MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != NULL;
}
bool snx_host_protect(unsigned char* base, uint64_t at, uint64_t size, unsigned permissions) {
    size_t page = snx_host_page();
    DWORD old;
    for (uint64_t done = 0; done < size; done += page)
        if (!VirtualProtect(base + at + done, page, protection(permissions), &old)) return false;
    return true;
}
bool snx_host_unmap(unsigned char* base, uint64_t at, uint64_t size) {
    size_t page = snx_host_page();
    bool ok = true;
    for (uint64_t done = 0; done < size; done += page)
        ok = g_host.unmap2(GetCurrentProcess(), base + at + done, MEM_PRESERVE_PLACEHOLDER) && ok;
    return ok;
}
#else
static int protection(unsigned permissions) {
    return (permissions & (SNX_HOST_READ | SNX_HOST_EXEC) ? PROT_READ : 0) |
           (permissions & SNX_HOST_WRITE ? PROT_WRITE : 0);
}
size_t snx_host_page(void) {
    long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? (size_t)page : 0;
}
bool snx_host_memory_new(SnxHostMemory* m, uint64_t size) {
    *m = (SnxHostMemory){0};
    int fd = -1;
#if defined(__linux__) || defined(__ANDROID__)
    /* the system call itself: Android's C library has memfd_create only from Android 11 (API 30), as code_loader.c
       says */
    fd = (int)syscall(SYS_memfd_create, "snx-host", 1u /* MFD_CLOEXEC */);
#else
    char name[64]; /* macOS takes 31 characters at most */
    snprintf(name, sizeof name, "/snx-%ld-%p", (long)getpid(), (void*)m);
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name);
#endif
    if (fd < 0) return false;
    void* view = MAP_FAILED;
    if (!ftruncate(fd, (off_t)size)) view = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        close(fd);
        return false;
    }
    m->handle = fd;
    m->view = view;
    m->size = size;
    return true;
}
void snx_host_memory_free(SnxHostMemory* m) {
    if (m->view) munmap(m->view, (size_t)m->size);
    if (m->view) close((int)m->handle);
    *m = (SnxHostMemory){0};
}
unsigned char* snx_host_reserve(uint64_t size) {
    void* base = mmap(NULL, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return base == MAP_FAILED ? NULL : base;
}
void snx_host_release(unsigned char* base, uint64_t size) { munmap(base, (size_t)size); }
bool snx_host_map(const SnxHostMemory* m, unsigned char* base, uint64_t at, uint64_t offset, uint64_t size,
                  unsigned permissions) {
    return mmap(base + at, (size_t)size, protection(permissions), MAP_SHARED | MAP_FIXED, (int)m->handle,
                (off_t)offset) != MAP_FAILED;
}
bool snx_host_commit(unsigned char* base, uint64_t at, uint64_t size) {
    return mmap(base + at, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
           MAP_FAILED;
}
bool snx_host_protect(unsigned char* base, uint64_t at, uint64_t size, unsigned permissions) {
    return !mprotect(base + at, (size_t)size, protection(permissions));
}
bool snx_host_unmap(unsigned char* base, uint64_t at, uint64_t size) {
    return mmap(base + at, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED;
}
#endif
