// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;

/** An image view that the user clicks: focusable, and clickable as each button is. */
public class ImageButton extends ImageView {
    public ImageButton(Context context) { this(context, null); }
    public ImageButton(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public ImageButton(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public ImageButton(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr);
        setFocusable(true);
        setClickable(true);
    }
    @Override public CharSequence getAccessibilityClassName() { return ImageButton.class.getName(); }
}
