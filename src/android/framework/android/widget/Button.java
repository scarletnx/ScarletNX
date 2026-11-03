// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.Gravity;

/** A text view that the user clicks: clickable and focusable, with its text in the center. */
public class Button extends TextView {
    public Button(Context context) { this(context, null); }
    public Button(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public Button(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public Button(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setClickable(true);
        setFocusable(true);
        setGravity(Gravity.CENTER);
    }
    @Override public CharSequence getAccessibilityClassName() { return Button.class.getName(); }
}
