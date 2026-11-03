// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

import android.os.Bundle;

/** An editor's description for the input method. */
public class EditorInfo implements android.text.InputType, android.os.Parcelable {
    public static final int IME_MASK_ACTION = 0xff, IME_ACTION_UNSPECIFIED = 0, IME_ACTION_NONE = 1, IME_ACTION_GO = 2,
        IME_ACTION_SEARCH = 3, IME_ACTION_SEND = 4, IME_ACTION_NEXT = 5, IME_ACTION_DONE = 6, IME_ACTION_PREVIOUS = 7,
        IME_FLAG_NO_PERSONALIZED_LEARNING = 0x1000000, IME_FLAG_NO_FULLSCREEN = 0x2000000, IME_FLAG_NAVIGATE_PREVIOUS = 0x4000000,
        IME_FLAG_NAVIGATE_NEXT = 0x8000000, IME_FLAG_NO_EXTRACT_UI = 0x10000000, IME_FLAG_NO_ACCESSORY_ACTION = 0x20000000,
        IME_FLAG_NO_ENTER_ACTION = 0x40000000, IME_FLAG_FORCE_ASCII = 0x80000000, IME_NULL = 0;
    public int inputType, imeOptions, actionId, initialSelStart = -1, initialSelEnd = -1, initialCapsMode, fieldId;
    public String privateImeOptions, packageName, fieldName;
    public CharSequence actionLabel, hintText, label;
    public Bundle extras;
    public android.os.LocaleList hintLocales;
    public String[] contentMimeTypes;

    private CharSequence surroundingText;
    private int surroundingOffset;

    public void setInitialSurroundingText(CharSequence sourceText) { setInitialSurroundingSubText(sourceText, 0); }
    public void setInitialSurroundingSubText(CharSequence subText, int subTextStart) {
        if (subText == null) throw new NullPointerException("subText");
        surroundingText = subText;
        surroundingOffset = subTextStart;
    }
    /** The selection in the surrounding text, or null when the text or the selection is not known. */
    private int[] selection() {
        if (surroundingText == null || initialSelStart < 0 || initialSelEnd < 0) return null;
        int a = Math.min(initialSelStart, initialSelEnd) - surroundingOffset, b = Math.max(initialSelStart, initialSelEnd) - surroundingOffset;
        return a < 0 || b > surroundingText.length() ? null : new int[] {a, b};
    }
    private static CharSequence part(CharSequence text, int start, int end, int flags) {
        CharSequence c = text.subSequence(start, end);
        return (flags & InputConnection.GET_TEXT_WITH_STYLES) != 0 ? c : c.toString();
    }
    public CharSequence getInitialTextBeforeCursor(int length, int flags) {
        int[] sel = selection();
        return sel == null ? null : part(surroundingText, Math.max(0, sel[0] - Math.max(0, length)), sel[0], flags);
    }
    public CharSequence getInitialSelectedText(int flags) {
        int[] sel = selection();
        return sel == null ? null : part(surroundingText, sel[0], sel[1], flags);
    }
    public CharSequence getInitialTextAfterCursor(int length, int flags) {
        int[] sel = selection();
        return sel == null ? null : part(surroundingText, sel[1], Math.min(surroundingText.length(), sel[1] + Math.max(0, length)), flags);
    }
    public void setSupportedHandwritingGestures(java.util.List<Class<? extends HandwritingGesture>> gestures) {}
    public void setSupportedHandwritingGesturePreviews(java.util.Set<Class<? extends PreviewableHandwritingGesture>> gestures) {}
    public void setStylusHandwritingEnabled(boolean enabled) {}
    public final void makeCompatible(int targetSdkVersion) {}
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
