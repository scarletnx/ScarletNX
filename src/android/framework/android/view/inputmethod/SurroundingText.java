// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

/** Text around the cursor. */
public final class SurroundingText {
    private final CharSequence text;
    private final int selectionStart, selectionEnd, offset;
    public SurroundingText(CharSequence text, int selectionStart, int selectionEnd, int offset) {
        this.text = text; this.selectionStart = selectionStart; this.selectionEnd = selectionEnd; this.offset = offset;
    }
    public CharSequence getText() { return text; }
    public int getSelectionStart() { return selectionStart; }
    public int getSelectionEnd() { return selectionEnd; }
    public int getOffset() { return offset; }
}
