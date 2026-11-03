// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;

/** A button with a checked state that each click toggles. */
public class CheckBox extends CompoundButton {
    public CheckBox(Context context) { this(context, null); }
    public CheckBox(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public CheckBox(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public CheckBox(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }
    @Override public CharSequence getAccessibilityClassName() { return CheckBox.class.getName(); }
}
