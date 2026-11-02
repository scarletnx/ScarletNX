// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

import android.os.Bundle;
import android.view.KeyEvent;
import android.view.View;

/** An input connection that sends key events to its view. */
public class BaseInputConnection implements InputConnection {
    protected final View mTargetView;

    public BaseInputConnection(View targetView, boolean fullEditor) { mTargetView = targetView; }
    @Override public CharSequence getTextBeforeCursor(int n, int flags) { return ""; }
    @Override public CharSequence getTextAfterCursor(int n, int flags) { return ""; }
    @Override public CharSequence getSelectedText(int flags) { return null; }
    @Override public int getCursorCapsMode(int reqModes) { return 0; }
    @Override public ExtractedText getExtractedText(ExtractedTextRequest request, int flags) { return null; }
    @Override public boolean deleteSurroundingText(int beforeLength, int afterLength) { return false; }
    @Override public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) { return false; }
    @Override public boolean setComposingText(CharSequence text, int newCursorPosition) { return false; }
    @Override public boolean setComposingRegion(int start, int end) { return false; }
    @Override public boolean finishComposingText() { return true; }
    @Override public boolean commitText(CharSequence text, int newCursorPosition) { return false; }
    @Override public boolean commitCompletion(CompletionInfo text) { return false; }
    @Override public boolean commitCorrection(CorrectionInfo correctionInfo) { return false; }
    @Override public boolean setSelection(int start, int end) { return false; }
    @Override public boolean performEditorAction(int editorAction) { return false; }
    @Override public boolean performContextMenuAction(int id) { return false; }
    @Override public boolean beginBatchEdit() { return false; }
    @Override public boolean endBatchEdit() { return false; }
    @Override public boolean sendKeyEvent(KeyEvent event) { return mTargetView != null && mTargetView.dispatchKeyEvent(event); }
    @Override public boolean clearMetaKeyStates(int states) { return false; }
    @Override public boolean reportFullscreenMode(boolean enabled) { return true; }
    @Override public boolean performPrivateCommand(String action, Bundle data) { return false; }
    @Override public boolean requestCursorUpdates(int cursorUpdateMode) { return false; }
    @Override public android.os.Handler getHandler() { return null; }
    @Override public void closeConnection() {}
    @Override public boolean commitContent(InputContentInfo inputContentInfo, int flags, Bundle opts) { return false; }
    public static int getComposingSpanStart(android.text.Spannable text) { return -1; }
    public static int getComposingSpanEnd(android.text.Spannable text) { return -1; }
}
