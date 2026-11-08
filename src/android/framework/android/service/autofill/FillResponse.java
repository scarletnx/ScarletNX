// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

import android.view.autofill.AutofillId;
import java.util.ArrayList;

/** A service's answer: its datasets, and the fields it ignores */
public final class FillResponse implements android.os.Parcelable {
    private final ArrayList<Dataset> datasets;
    private FillResponse(Builder b) { datasets = new ArrayList<>(b.datasets); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }

    public static final class Builder {
        private final ArrayList<Dataset> datasets = new ArrayList<>();
        public Builder() {}
        public Builder addDataset(Dataset dataset) { if (dataset != null) datasets.add(dataset); return this; }
        public Builder setIgnoredIds(AutofillId... ids) { return this; }
        public Builder setClientState(android.os.Bundle clientState) { return this; }
        public Builder setAuthentication(AutofillId[] ids, android.content.IntentSender authentication, android.widget.RemoteViews presentation) { return this; }
        public Builder setFlags(int flags) { return this; }
        public FillResponse build() { return new FillResponse(this); }
    }
}
