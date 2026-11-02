// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Whether a text is right to left. */
public interface TextDirectionHeuristic {
    boolean isRtl(char[] array, int start, int count);

    boolean isRtl(CharSequence cs, int start, int count);
}
