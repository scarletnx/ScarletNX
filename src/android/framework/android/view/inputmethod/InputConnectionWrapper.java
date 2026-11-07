// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

import android.os.Bundle;
import android.os.Handler;
import android.view.KeyEvent;

/** An InputConnection that passes each call to another one. */
public class InputConnectionWrapper implements InputConnection {
    private InputConnection target;

    public InputConnectionWrapper(InputConnection target, boolean mutable) { this.target = target; }
    public void setTarget(InputConnection target) { this.target = target; }

    @Override public CharSequence getTextBeforeCursor(int n, int flags) { return target.getTextBeforeCursor(n, flags); }
    @Override public CharSequence getTextAfterCursor(int n, int flags) { return target.getTextAfterCursor(n, flags); }
    @Override public CharSequence getSelectedText(int flags) { return target.getSelectedText(flags); }
    @Override public SurroundingText getSurroundingText(int beforeLength, int afterLength, int flags) { return target.getSurroundingText(beforeLength, afterLength, flags); }
    @Override public int getCursorCapsMode(int reqModes) { return target.getCursorCapsMode(reqModes); }
    @Override public ExtractedText getExtractedText(ExtractedTextRequest request, int flags) { return target.getExtractedText(request, flags); }
    @Override public boolean deleteSurroundingText(int beforeLength, int afterLength) { return target.deleteSurroundingText(beforeLength, afterLength); }
    @Override public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) { return target.deleteSurroundingTextInCodePoints(beforeLength, afterLength); }
    @Override public boolean setComposingText(CharSequence text, int newCursorPosition) { return target.setComposingText(text, newCursorPosition); }
    @Override public boolean setComposingRegion(int start, int end) { return target.setComposingRegion(start, end); }
    @Override public boolean finishComposingText() { return target.finishComposingText(); }
    @Override public boolean commitText(CharSequence text, int newCursorPosition) { return target.commitText(text, newCursorPosition); }
    @Override public boolean commitCompletion(CompletionInfo text) { return target.commitCompletion(text); }
    @Override public boolean commitCorrection(CorrectionInfo correctionInfo) { return target.commitCorrection(correctionInfo); }
    @Override public boolean setSelection(int start, int end) { return target.setSelection(start, end); }
    @Override public boolean performEditorAction(int editorAction) { return target.performEditorAction(editorAction); }
    @Override public boolean performContextMenuAction(int id) { return target.performContextMenuAction(id); }
    @Override public boolean beginBatchEdit() { return target.beginBatchEdit(); }
    @Override public boolean endBatchEdit() { return target.endBatchEdit(); }
    @Override public boolean sendKeyEvent(KeyEvent event) { return target.sendKeyEvent(event); }
    @Override public boolean clearMetaKeyStates(int states) { return target.clearMetaKeyStates(states); }
    @Override public boolean reportFullscreenMode(boolean enabled) { return target.reportFullscreenMode(enabled); }
    @Override public boolean performSpellCheck() { return target.performSpellCheck(); }
    @Override public boolean performPrivateCommand(String action, Bundle data) { return target.performPrivateCommand(action, data); }
    @Override public void performHandwritingGesture(HandwritingGesture gesture, java.util.concurrent.Executor executor, java.util.function.IntConsumer consumer) {
        target.performHandwritingGesture(gesture, executor, consumer);
    }
    @Override public boolean previewHandwritingGesture(PreviewableHandwritingGesture gesture, android.os.CancellationSignal cancellationSignal) {
        return target.previewHandwritingGesture(gesture, cancellationSignal);
    }
    @Override public boolean requestCursorUpdates(int cursorUpdateMode) { return target.requestCursorUpdates(cursorUpdateMode); }
    @Override public boolean requestCursorUpdates(int cursorUpdateMode, int cursorUpdateFilter) { return target.requestCursorUpdates(cursorUpdateMode, cursorUpdateFilter); }
    @Override public Handler getHandler() { return target.getHandler(); }
    @Override public void closeConnection() { target.closeConnection(); }
    @Override public boolean commitContent(InputContentInfo inputContentInfo, int flags, Bundle opts) { return target.commitContent(inputContentInfo, flags, opts); }
    @Override public boolean setImeConsumesInput(boolean imeConsumesInput) { return target.setImeConsumesInput(imeConsumesInput); }
}
