// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.res.Resources;

/** A spinner adapter that inflates its drop-down views with a theme. */
public interface ThemedSpinnerAdapter extends SpinnerAdapter {
    void setDropDownViewTheme(Resources.Theme theme);
    Resources.Theme getDropDownViewTheme();
}
