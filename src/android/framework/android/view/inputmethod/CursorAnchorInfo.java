// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

/** Where an editor's cursor and characters are, for the input method. */
public final class CursorAnchorInfo {
    public static final int FLAG_HAS_VISIBLE_REGION = 1, FLAG_HAS_INVISIBLE_REGION = 2, FLAG_IS_RTL = 4;

    public static final class Builder {
        public Builder setSelectionRange(int newStart, int newEnd) { return this; }
        public Builder setComposingText(int composingTextStart, CharSequence composingText) { return this; }
        public Builder setInsertionMarkerLocation(float horizontalPosition, float lineTop, float lineBaseline, float lineBottom, int flags) { return this; }
        public Builder addCharacterBounds(int index, float left, float top, float right, float bottom, int flags) { return this; }
        public Builder setMatrix(android.graphics.Matrix matrix) { return this; }
        public Builder setEditorBoundsInfo(EditorBoundsInfo bounds) { return this; }
        public Builder addVisibleLineBounds(float left, float top, float right, float bottom) { return this; }
        public Builder clearVisibleLineBounds() { return this; }
        public Builder setTextAppearanceInfo(Object textAppearanceInfo) { return this; }
        public void reset() {}
        public CursorAnchorInfo build() { return new CursorAnchorInfo(); }
    }
}
