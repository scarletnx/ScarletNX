// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

import android.widget.inline.InlinePresentationSpec;
import java.util.List;

/** A keyboard's request for inline suggestions: none comes, as no keyboard asks */
public final class InlineSuggestionsRequest implements android.os.Parcelable {
    private final int max;
    private final List<InlinePresentationSpec> specs;
    InlineSuggestionsRequest(int max, List<InlinePresentationSpec> specs) {
        this.max = max;
        this.specs = specs;
    }
    public int getMaxSuggestionCount() { return max; }
    public List<InlinePresentationSpec> getInlinePresentationSpecs() { return specs; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
