// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;

/** A span that changes how its characters draw. */
public abstract class CharacterStyle {
    public abstract void updateDrawState(TextPaint tp);

    public static CharacterStyle wrap(CharacterStyle cs) { return cs; }

    public CharacterStyle getUnderlying() { return this; }
}
