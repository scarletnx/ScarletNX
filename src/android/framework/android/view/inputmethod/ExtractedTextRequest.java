// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

/** What extracted text an input method asks for. */
public class ExtractedTextRequest implements android.os.Parcelable {
    public int token, flags, hintMaxLines, hintMaxChars;
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int f) { dest.writeValue(this); }
}
