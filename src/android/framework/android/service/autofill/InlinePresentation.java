// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

import android.app.slice.Slice;
import android.widget.inline.InlinePresentationSpec;

/** A suggestion shown in the keyboard's strip */
public final class InlinePresentation implements android.os.Parcelable {
    private final Slice slice;
    private final InlinePresentationSpec spec;
    private final boolean pinned;
    public InlinePresentation(Slice slice, InlinePresentationSpec inlinePresentationSpec, boolean pinned) {
        this.slice = slice;
        this.spec = inlinePresentationSpec;
        this.pinned = pinned;
    }
    public Slice getSlice() { return slice; }
    public InlinePresentationSpec getInlinePresentationSpec() { return spec; }
    public boolean isPinned() { return pinned; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
