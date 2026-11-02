// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.translation;

/** A request to translate a view's text. */
public final class ViewTranslationRequest {
    public static final String ID_TEXT = "android:text";
    private ViewTranslationRequest() {}
    public static final class Builder {
        public Builder(android.view.autofill.AutofillId autofillId) {}
        public Builder(android.view.autofill.AutofillId autofillId, long virtualChildId) {}
        public Builder setValue(String key, TranslationRequestValue value) { return this; }
        public ViewTranslationRequest build() { return new ViewTranslationRequest(); }
    }
}
