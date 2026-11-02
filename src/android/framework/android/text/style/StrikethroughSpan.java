// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;

/** A line through text. */
public class StrikethroughSpan extends CharacterStyle implements UpdateAppearance {
    public StrikethroughSpan() {}
    @Override public void updateDrawState(TextPaint ds) { ds.setStrikeThruText(true); }
}
