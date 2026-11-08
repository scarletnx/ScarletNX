// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Told when an Editable's text changes. */
public interface TextWatcher extends NoCopySpan {
    void beforeTextChanged(CharSequence s, int start, int count, int after);
    void onTextChanged(CharSequence s, int start, int before, int count);
    void afterTextChanged(Editable s);
}
