// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget.inline;

import android.util.Size;

/** The size and style of an inline suggestion */
public final class InlinePresentationSpec implements android.os.Parcelable {
    private final Size min, max;
    private final android.os.Bundle style;
    private InlinePresentationSpec(Size min, Size max, android.os.Bundle style) {
        this.min = min;
        this.max = max;
        this.style = style;
    }
    public Size getMinSize() { return min; }
    public Size getMaxSize() { return max; }
    public android.os.Bundle getStyle() { return style; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
    public static final class Builder {
        private final Size min, max;
        private android.os.Bundle style = android.os.Bundle.EMPTY;
        public Builder(Size minSize, Size maxSize) { min = minSize; max = maxSize; }
        public Builder setStyle(android.os.Bundle style) { this.style = style; return this; }
        public InlinePresentationSpec build() { return new InlinePresentationSpec(min, max, style); }
    }
}
