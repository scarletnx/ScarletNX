// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

import java.util.List;

/** A request to save what the user typed */
public final class SaveRequest implements android.os.Parcelable {
    private final List<FillContext> contexts;
    SaveRequest(List<FillContext> contexts) { this.contexts = contexts; }
    public List<FillContext> getFillContexts() { return contexts; }
    public android.os.Bundle getClientState() { return null; }
    public List<String> getDatasetIds() { return null; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
