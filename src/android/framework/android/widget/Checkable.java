// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

/** A view with a checked state. */
public interface Checkable {
    void setChecked(boolean checked);
    boolean isChecked();
    void toggle();
}
