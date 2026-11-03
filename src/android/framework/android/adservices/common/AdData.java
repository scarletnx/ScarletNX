// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.common;

import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;

/** An ad: its render URI and metadata. */
public final class AdData implements Parcelable {
    private final Uri renderUri;
    private final String metadata;

    private AdData(Uri renderUri, String metadata) { this.renderUri = renderUri; this.metadata = metadata; }
    public Uri getRenderUri() { return renderUri; }
    public String getMetadata() { return metadata; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<AdData> CREATOR = new Parcelable.Creator<>() {
        @Override public AdData createFromParcel(Parcel in) { return (AdData) in.readValue(null); }
        @Override public AdData[] newArray(int size) { return new AdData[size]; }
    };

    public static final class Builder {
        private Uri renderUri;
        private String metadata;
        public Builder() {}
        public Builder setRenderUri(Uri renderUri) { this.renderUri = renderUri; return this; }
        public Builder setMetadata(String metadata) { this.metadata = metadata; return this; }
        public AdData build() { return new AdData(renderUri, metadata); }
    }
}
