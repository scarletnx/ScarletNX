// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

/** An editor's text, as the input method sees it. */
public class ExtractedText implements android.os.Parcelable {
    public static final int FLAG_SINGLE_LINE = 1, FLAG_SELECTING = 2;
    public CharSequence text, hint;
    public int startOffset, partialStartOffset, partialEndOffset, selectionStart, selectionEnd, flags;
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int f) { dest.writeValue(this); }
}
