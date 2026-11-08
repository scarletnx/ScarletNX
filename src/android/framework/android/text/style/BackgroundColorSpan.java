// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;

/** A color behind text. */
public class BackgroundColorSpan extends CharacterStyle implements UpdateAppearance {
    private final int color;

    public BackgroundColorSpan(int color) { this.color = color; }
    public int getBackgroundColor() { return color; }
    @Override public void updateDrawState(TextPaint tp) { tp.bgColor = color; }
}
