// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;

/** A text color. */
public class ForegroundColorSpan extends CharacterStyle implements UpdateAppearance {
    private final int color;

    public ForegroundColorSpan(int color) { this.color = color; }
    public int getForegroundColor() { return color; }
    @Override public void updateDrawState(TextPaint tp) { tp.setColor(color); }
}
