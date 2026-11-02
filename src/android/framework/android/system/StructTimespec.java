// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.system;

/** A time in seconds and nanoseconds. */
public final class StructTimespec {
    public final long tv_sec, tv_nsec;
    public StructTimespec(long tv_sec, long tv_nsec) { this.tv_sec = tv_sec; this.tv_nsec = tv_nsec; }
}
