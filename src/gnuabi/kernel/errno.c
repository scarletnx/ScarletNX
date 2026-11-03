/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A host errno as the guest's (kernel.h): never the host's numbering, which Windows' differs from */
#include "gnuabi/kernel/kernel.h"
#include <errno.h>

int gnu_host_errno(int error) {
    switch (error) {
        case EPERM: return -GNU_EPERM;
        case ENOENT: return -GNU_ENOENT;
        case ESRCH: return -GNU_ESRCH;
        case EINTR: return -GNU_EINTR;
        case EIO: return -GNU_EIO;
        case ENXIO: return -GNU_ENXIO;
        case E2BIG: return -GNU_E2BIG;
        case ENOEXEC: return -GNU_ENOEXEC;
        case EBADF: return -GNU_EBADF;
        case ECHILD: return -GNU_ECHILD;
        case EAGAIN: return -GNU_EAGAIN;
        case ENOMEM: return -GNU_ENOMEM;
        case EACCES: return -GNU_EACCES;
        case EFAULT: return -GNU_EFAULT;
        case EBUSY: return -GNU_EBUSY;
        case EEXIST: return -GNU_EEXIST;
        case EXDEV: return -GNU_EXDEV;
        case ENODEV: return -GNU_ENODEV;
        case ENOTDIR: return -GNU_ENOTDIR;
        case EISDIR: return -GNU_EISDIR;
        case EINVAL: return -GNU_EINVAL;
        case ENFILE: return -GNU_ENFILE;
        case EMFILE: return -GNU_EMFILE;
        case ENOTTY: return -GNU_ENOTTY;
        case EFBIG: return -GNU_EFBIG;
        case ENOSPC: return -GNU_ENOSPC;
        case ESPIPE: return -GNU_ESPIPE;
        case EROFS: return -GNU_EROFS;
        case EMLINK: return -GNU_EMLINK;
        case EPIPE: return -GNU_EPIPE;
        case ERANGE: return -GNU_ERANGE;
        case EDEADLK: return -GNU_EDEADLK;
        case ENAMETOOLONG: return -GNU_ENAMETOOLONG;
        case ENOSYS: return -GNU_ENOSYS;
        case ENOTEMPTY: return -GNU_ENOTEMPTY;
#ifdef ETXTBSY
        case ETXTBSY: return -GNU_ETXTBSY;
#endif
#ifdef ELOOP
        case ELOOP: return -GNU_ELOOP;
#endif
#ifdef EOVERFLOW
        case EOVERFLOW: return -GNU_EOVERFLOW;
#endif
#ifdef ENOTSUP
        case ENOTSUP: return -GNU_ENOTSUP;
#endif
#ifdef ETIMEDOUT
        case ETIMEDOUT: return -GNU_ETIMEDOUT;
#endif
        default: return -GNU_EIO;
    }
}
