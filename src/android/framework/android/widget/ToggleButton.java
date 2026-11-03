// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;

/** A button that shows one text when checked and another when not. */
public class ToggleButton extends CompoundButton {
    private CharSequence textOn = "ON", textOff = "OFF";

    public ToggleButton(Context context) { this(context, null); }
    public ToggleButton(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public ToggleButton(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public ToggleButton(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); syncText(); }

    @Override public void setChecked(boolean checked) { super.setChecked(checked); syncText(); }
    private void syncText() { CharSequence t = isChecked() ? textOn : textOff; if (t != null) setText(t); }
    public CharSequence getTextOn() { return textOn; }
    public void setTextOn(CharSequence textOn) { this.textOn = textOn; }
    public CharSequence getTextOff() { return textOff; }
    public void setTextOff(CharSequence textOff) { this.textOff = textOff; }
    @Override public CharSequence getAccessibilityClassName() { return ToggleButton.class.getName(); }
}
