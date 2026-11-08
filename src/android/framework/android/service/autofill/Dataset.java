// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

import android.view.autofill.AutofillId;
import android.view.autofill.AutofillValue;
import android.widget.RemoteViews;
import java.util.ArrayList;

/** The values a service offers for a screen's fields */
public final class Dataset implements android.os.Parcelable {
    private final ArrayList<AutofillId> ids;
    private final ArrayList<AutofillValue> values;
    private final android.content.IntentSender authentication;
    private final String id;

    private Dataset(Builder b) {
        ids = new ArrayList<>(b.ids);
        values = new ArrayList<>(b.values);
        authentication = b.authentication;
        id = b.id;
    }
    public String getId() { return id; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }

    public static final class Builder {
        private final ArrayList<AutofillId> ids = new ArrayList<>();
        private final ArrayList<AutofillValue> values = new ArrayList<>();
        private android.content.IntentSender authentication;
        private String id;
        private boolean built;

        public Builder() {}
        public Builder(RemoteViews presentation) {}
        public Builder(Presentations presentations) {}
        public Builder setAuthentication(android.content.IntentSender authentication) { this.authentication = authentication; return this; }
        public Builder setId(String id) { this.id = id; return this; }
        public Builder setInlinePresentation(InlinePresentation inlinePresentation) { return this; }
        public Builder setValue(AutofillId id, AutofillValue value) { ids.add(id); values.add(value); return this; }
        public Builder setValue(AutofillId id, AutofillValue value, RemoteViews presentation) { return setValue(id, value); }
        public Builder setValue(AutofillId id, AutofillValue value, RemoteViews presentation, InlinePresentation inlinePresentation) {
            return setValue(id, value);
        }
        public Builder setField(AutofillId id, Field field) { ids.add(id); values.add(null); return this; }
        public Dataset build() {
            if (built) throw new IllegalStateException("already built");
            if (ids.isEmpty()) throw new IllegalStateException("at least one value must be set");
            built = true;
            return new Dataset(this);
        }
    }
}
