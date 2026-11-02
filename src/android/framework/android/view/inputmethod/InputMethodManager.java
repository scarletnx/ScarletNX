// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

import android.os.IBinder;
import android.view.View;

/** The input method: no on-screen keyboard; the host keyboard sends key events to the focused view. */
public final class InputMethodManager {
    public static final int SHOW_IMPLICIT = 1, SHOW_FORCED = 2, HIDE_IMPLICIT_ONLY = 1, HIDE_NOT_ALWAYS = 2,
        RESULT_UNCHANGED_SHOWN = 0, RESULT_UNCHANGED_HIDDEN = 1, RESULT_SHOWN = 2, RESULT_HIDDEN = 3;

    public boolean isActive(View view) { return view != null && view.hasFocus(); }
    public boolean isActive() { return false; }
    public boolean isAcceptingText() { return false; }
    public boolean showSoftInput(View view, int flags) { return false; }
    public boolean showSoftInput(View view, int flags, android.os.ResultReceiver resultReceiver) { return false; }
    public boolean hideSoftInputFromWindow(IBinder windowToken, int flags) { return false; }
    public boolean hideSoftInputFromWindow(IBinder windowToken, int flags, android.os.ResultReceiver resultReceiver) { return false; }
    public void restartInput(View view) {}
    public void updateSelection(View view, int selStart, int selEnd, int candidatesStart, int candidatesEnd) {}
    public void updateCursorAnchorInfo(View view, CursorAnchorInfo cursorAnchorInfo) {}
    public void updateExtractedText(View view, int token, ExtractedText text) {}
    public void viewClicked(View view) {}
    public void toggleSoftInput(int showFlags, int hideFlags) {}
    public void startStylusHandwriting(View view) {}
    public boolean isStylusHandwritingAvailable() { return false; }
    public void displayCompletions(View view, CompletionInfo[] completions) {}
    public java.util.List<InputMethodInfo> getEnabledInputMethodList() { return java.util.Collections.emptyList(); }
    public java.util.List<InputMethodInfo> getInputMethodList() { return java.util.Collections.emptyList(); }
    public InputMethodSubtype getCurrentInputMethodSubtype() { return null; }
    public boolean isFullscreenMode() { return false; }
    public void sendAppPrivateCommand(View view, String action, android.os.Bundle data) {}
    public void invalidateInput(View view) {}
}
