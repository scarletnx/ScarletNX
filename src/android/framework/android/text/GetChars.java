// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Text that copies its chars out in one call. */
public interface GetChars extends CharSequence {
    void getChars(int start, int end, char[] dest, int destoff);
}
