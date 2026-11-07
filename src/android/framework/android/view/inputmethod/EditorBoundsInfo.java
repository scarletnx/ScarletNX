// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

/** An editor's bounds, for the input method. */
public final class EditorBoundsInfo {
    public static final class Builder {
        public Builder setEditorBounds(android.graphics.RectF bounds) { return this; }
        public Builder setHandwritingBounds(android.graphics.RectF bounds) { return this; }
        public Builder setTopVisibleLine(int line) { return this; }
        public EditorBoundsInfo build() { return new EditorBoundsInfo(); }
    }
}
