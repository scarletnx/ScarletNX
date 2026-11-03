// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.method;

import android.graphics.Rect;
import android.text.Editable;
import android.text.GetChars;
import android.text.TextWatcher;
import android.view.View;

/** Shows each char of a password as a dot. The last typed char is not shown, as with Android's "show passwords" off. */
public class PasswordTransformationMethod implements TransformationMethod, TextWatcher {
    private static final char DOT = '•';
    private static PasswordTransformationMethod instance;

    public PasswordTransformationMethod() {}
    public static PasswordTransformationMethod getInstance() {
        if (instance == null) instance = new PasswordTransformationMethod();
        return instance;
    }

    /** A view of the source with each char as a dot: it follows the source as it changes. */
    private static final class Dots implements CharSequence, GetChars {
        private final CharSequence source;

        Dots(CharSequence source) { this.source = source; }
        @Override public int length() { return source.length(); }
        @Override public char charAt(int i) { source.charAt(i); return DOT; }
        @Override public CharSequence subSequence(int start, int end) { return String.valueOf(DOT).repeat(Math.max(0, end - start)); }
        @Override public String toString() { return subSequence(0, length()).toString(); }
        @Override public void getChars(int start, int end, char[] dest, int off) { java.util.Arrays.fill(dest, off, off + end - start, DOT); }
    }

    @Override public CharSequence getTransformation(CharSequence source, View view) { return new Dots(source); }
    @Override public void onFocusChanged(View view, CharSequence sourceText, boolean focused, int direction, Rect previouslyFocusedRect) {}
    @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
    @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
    @Override public void afterTextChanged(Editable s) {}
}
