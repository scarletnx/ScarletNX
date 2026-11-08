// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.measurement;

import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;

/** One registration URI of a web trigger registration request. */
public final class WebTriggerParams implements Parcelable {
    private final Uri registrationUri;
    private final boolean debugKeyAllowed;

    private WebTriggerParams(Uri uri, boolean debugKeyAllowed) { registrationUri = uri; this.debugKeyAllowed = debugKeyAllowed; }
    public Uri getRegistrationUri() { return registrationUri; }
    public boolean isDebugKeyAllowed() { return debugKeyAllowed; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<WebTriggerParams> CREATOR = new Parcelable.Creator<>() {
        @Override public WebTriggerParams createFromParcel(Parcel in) { return (WebTriggerParams) in.readValue(null); }
        @Override public WebTriggerParams[] newArray(int size) { return new WebTriggerParams[size]; }
    };

    public static final class Builder {
        private final Uri registrationUri;
        private boolean debugKeyAllowed;
        public Builder(Uri registrationUri) { this.registrationUri = registrationUri; }
        public Builder setDebugKeyAllowed(boolean debugKeyAllowed) { this.debugKeyAllowed = debugKeyAllowed; return this; }
        public WebTriggerParams build() { return new WebTriggerParams(registrationUri, debugKeyAllowed); }
    }
}
