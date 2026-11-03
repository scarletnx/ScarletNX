// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.method;

import android.graphics.Rect;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.view.View;

/** Shows each original char as its replacement char. The result is a copy that keeps the spans of the source. */
public abstract class ReplacementTransformationMethod implements TransformationMethod {
    public ReplacementTransformationMethod() {}

    protected abstract char[] getOriginal();

    protected abstract char[] getReplacement();

    @Override public CharSequence getTransformation(CharSequence source, View v) {
        char[] original = getOriginal(), replacement = getReplacement();
        char[] chars = source.toString().toCharArray();
        boolean changed = false;
        for (int i = 0; i < chars.length; ++i) {
            for (int k = 0; k < original.length; ++k) {
                if (chars[i] == original[k]) {
                    chars[i] = replacement[k];
                    changed = true;
                    break;
                }
            }
        }
        if (!changed) return source;
        if (!(source instanceof Spanned)) return new String(chars);
        SpannableStringBuilder out = new SpannableStringBuilder(source);
        for (int i = 0; i < chars.length; ++i) if (chars[i] != source.charAt(i)) out.replace(i, i + 1, String.valueOf(chars[i]));
        return out;
    }
    @Override public void onFocusChanged(View view, CharSequence sourceText, boolean focused, int direction, Rect previouslyFocusedRect) {}
}
