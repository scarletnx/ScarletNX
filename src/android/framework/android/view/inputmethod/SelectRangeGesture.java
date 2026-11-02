// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

/** A stylus gesture over a range of text. */
public final class SelectRangeGesture extends PreviewableHandwritingGesture {
    public int getGranularity() { return 1; }
    public android.graphics.RectF getSelectionArea() { return new android.graphics.RectF(); }
    public android.graphics.RectF getDeletionArea() { return new android.graphics.RectF(); }
    public android.graphics.RectF getSelectionStartArea() { return new android.graphics.RectF(); }
    public android.graphics.RectF getSelectionEndArea() { return new android.graphics.RectF(); }
    public android.graphics.RectF getDeletionStartArea() { return new android.graphics.RectF(); }
    public android.graphics.RectF getDeletionEndArea() { return new android.graphics.RectF(); }
}
