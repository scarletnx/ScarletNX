// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

import android.app.assist.AssistStructure;

/** A screen to fill: its structure */
public final class FillContext implements android.os.Parcelable {
    private final int requestId;
    private final AssistStructure structure;
    FillContext(int requestId, AssistStructure structure) {
        this.requestId = requestId;
        this.structure = structure;
    }
    public int getRequestId() { return requestId; }
    public AssistStructure getStructure() { return structure; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
