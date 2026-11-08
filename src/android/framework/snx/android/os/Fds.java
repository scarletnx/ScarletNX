// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.os;

import java.io.FileDescriptor;

/** The host descriptors of java.io.FileDescriptor, which the runtime reads and makes (runtime/fds.c). */
public final class Fds {
    private Fds() {}

    /** the descriptor, -1 for none */
    public static native int of(FileDescriptor fd);
    /** a FileDescriptor of the descriptor, which owns it from now on */
    public static native FileDescriptor wrap(int fd);
    /** a second descriptor of the same open file, or -errno */
    public static native int dup(int fd);
    /** 0 or -errno */
    public static native int close(int fd);
    /** the size of a regular file, else -1 */
    public static native long size(int fd);
}
