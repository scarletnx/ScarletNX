/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A host errno as the guest's and back (kernel.h): never the host's numbering, which Windows', FreeBSD's and macOS's
   differ from. The names are the network stack's table (Linux's values) and the few it leaves out */
#include "gnuabi/kernel/kernel.h"
#include <errno.h>

/* the names a host's errno.h lacks (Windows' CRT has no ESHUTDOWN...): numbers no host errno is, so that no host
   error becomes them and the guest's become an unknown error */
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT (-1)
#endif
#ifndef EPFNOSUPPORT
#define EPFNOSUPPORT (-2)
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN (-3)
#endif
#ifndef ETOOMANYREFS
#define ETOOMANYREFS (-4)
#endif
#ifndef EHOSTDOWN
#define EHOSTDOWN (-5)
#endif
#ifndef ESTALE
#define ESTALE (-6)
#endif
#ifndef EDQUOT
#define EDQUOT (-7)
#endif

static const struct {
    int host, guest;
} table[] = {
#define SNX_NET_ERRNO(name, value) {E##name, value},
#include "common/network/abi/errno.inc"
    {ENOEXEC, GNU_ENOEXEC},
    {EXDEV, GNU_EXDEV},
    {EMLINK, GNU_EMLINK},
    {EDEADLK, GNU_EDEADLK},
    {ENOTEMPTY, GNU_ENOTEMPTY},
#ifdef ETXTBSY
    {ETXTBSY, GNU_ETXTBSY},
#endif
#ifdef ENOTSUP
    {ENOTSUP, GNU_ENOTSUP},
#endif
#ifdef EWOULDBLOCK
    {EWOULDBLOCK, GNU_EAGAIN},
#endif
};

int gnu_host_errno(int error) {
    for (size_t i = 0; i < sizeof table / sizeof *table; ++i)
        if (table[i].host == error) return -table[i].guest;
    return -GNU_EIO;
}

int gnu_errno_to_host(int guest) {
    for (size_t i = 0; i < sizeof table / sizeof *table; ++i)
        if (table[i].guest == guest) return table[i].host;
    return EIO;
}
