// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;

/** A button that a click checks but does not clear: its RadioGroup clears the others. */
public class RadioButton extends CompoundButton {
    public RadioButton(Context context) { this(context, null); }
    public RadioButton(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public RadioButton(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public RadioButton(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }
    @Override public void toggle() { if (!isChecked()) super.toggle(); }
    @Override public CharSequence getAccessibilityClassName() { return RadioButton.class.getName(); }
}
