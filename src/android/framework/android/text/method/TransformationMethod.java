// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.method;

import android.graphics.Rect;
import android.view.View;

/** Changes the text that a TextView shows, for example to hide a password. */
public interface TransformationMethod {
    CharSequence getTransformation(CharSequence source, View view);

    void onFocusChanged(View view, CharSequence sourceText, boolean focused, int direction, Rect previouslyFocusedRect);
}
