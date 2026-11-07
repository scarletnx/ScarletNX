// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

import android.os.Bundle;
import android.view.KeyEvent;

/** An editor's connection to the input method. */
public interface InputConnection {
    int GET_TEXT_WITH_STYLES = 1, GET_EXTRACTED_TEXT_MONITOR = 1, INPUT_CONTENT_GRANT_READ_URI_PERMISSION = 1,
        CURSOR_UPDATE_IMMEDIATE = 1, CURSOR_UPDATE_MONITOR = 2, CURSOR_UPDATE_FILTER_EDITOR_BOUNDS = 4,
        CURSOR_UPDATE_FILTER_CHARACTER_BOUNDS = 8, CURSOR_UPDATE_FILTER_INSERTION_MARKER = 16,
        CURSOR_UPDATE_FILTER_VISIBLE_LINE_BOUNDS = 32, CURSOR_UPDATE_FILTER_TEXT_APPEARANCE = 64;
    CharSequence getTextBeforeCursor(int n, int flags);
    CharSequence getTextAfterCursor(int n, int flags);
    CharSequence getSelectedText(int flags);
    int getCursorCapsMode(int reqModes);
    ExtractedText getExtractedText(ExtractedTextRequest request, int flags);
    boolean deleteSurroundingText(int beforeLength, int afterLength);
    boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength);
    boolean setComposingText(CharSequence text, int newCursorPosition);
    boolean setComposingRegion(int start, int end);
    boolean finishComposingText();
    boolean commitText(CharSequence text, int newCursorPosition);
    boolean commitCompletion(CompletionInfo text);
    boolean commitCorrection(CorrectionInfo correctionInfo);
    boolean setSelection(int start, int end);
    boolean performEditorAction(int editorAction);
    boolean performContextMenuAction(int id);
    boolean beginBatchEdit();
    boolean endBatchEdit();
    boolean sendKeyEvent(KeyEvent event);
    boolean clearMetaKeyStates(int states);
    boolean reportFullscreenMode(boolean enabled);
    boolean performPrivateCommand(String action, Bundle data);
    boolean requestCursorUpdates(int cursorUpdateMode);
    android.os.Handler getHandler();
    void closeConnection();
    boolean commitContent(InputContentInfo inputContentInfo, int flags, Bundle opts);
    default boolean requestCursorUpdates(int cursorUpdateMode, int cursorUpdateFilter) { return requestCursorUpdates(cursorUpdateMode | cursorUpdateFilter); }
    default void performHandwritingGesture(HandwritingGesture gesture, java.util.concurrent.Executor executor, java.util.function.IntConsumer consumer) {}
    default boolean previewHandwritingGesture(PreviewableHandwritingGesture gesture, android.os.CancellationSignal cancellationSignal) { return false; }
    default boolean performSpellCheck() { return false; }
    default boolean setImeConsumesInput(boolean imeConsumesInput) { return false; }
    default SurroundingText getSurroundingText(int beforeLength, int afterLength, int flags) { return null; }
}
