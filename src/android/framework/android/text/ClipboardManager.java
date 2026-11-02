// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** The old clipboard API: text only. */
@Deprecated
public abstract class ClipboardManager {
    public abstract CharSequence getText();
    public abstract void setText(CharSequence text);
    public abstract boolean hasText();
}
