// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.translation;

/** A text to translate. */
public final class TranslationRequestValue {
    private TranslationRequestValue() {}
    public static TranslationRequestValue forText(CharSequence text) { return new TranslationRequestValue(); }
}
