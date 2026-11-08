// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

import java.util.List;

/** A request to fill a screen */
public final class FillRequest implements android.os.Parcelable {
    public static final int FLAG_MANUAL_REQUEST = 1, FLAG_COMPATIBILITY_MODE_REQUEST = 2;
    private final int id, flags;
    private final List<FillContext> contexts;
    FillRequest(int id, List<FillContext> contexts, int flags) {
        this.id = id;
        this.contexts = contexts;
        this.flags = flags;
    }
    public int getId() { return id; }
    public List<FillContext> getFillContexts() { return contexts; }
    public android.os.Bundle getClientState() { return null; }
    public int getFlags() { return flags; }
    public android.view.inputmethod.InlineSuggestionsRequest getInlineSuggestionsRequest() { return null; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
