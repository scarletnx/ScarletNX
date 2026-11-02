// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;

/** A line under text. */
public class UnderlineSpan extends CharacterStyle implements UpdateAppearance {
    public UnderlineSpan() {}
    @Override public void updateDrawState(TextPaint ds) { ds.setUnderlineText(true); }
}
