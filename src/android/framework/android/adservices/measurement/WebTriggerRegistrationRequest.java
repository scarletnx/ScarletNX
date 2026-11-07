// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.measurement;

import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.List;

/** A request to register triggers from the web. */
public final class WebTriggerRegistrationRequest implements Parcelable {
    private final List<WebTriggerParams> params;
    private final Uri destination;

    private WebTriggerRegistrationRequest(List<WebTriggerParams> params, Uri destination) { this.params = params; this.destination = destination; }
    public List<WebTriggerParams> getTriggerParams() { return params; }
    public Uri getDestination() { return destination; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<WebTriggerRegistrationRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public WebTriggerRegistrationRequest createFromParcel(Parcel in) { return (WebTriggerRegistrationRequest) in.readValue(null); }
        @Override public WebTriggerRegistrationRequest[] newArray(int size) { return new WebTriggerRegistrationRequest[size]; }
    };

    public static final class Builder {
        private final List<WebTriggerParams> params;
        private final Uri destination;
        public Builder(List<WebTriggerParams> webTriggerParams, Uri destination) { params = webTriggerParams; this.destination = destination; }
        public WebTriggerRegistrationRequest build() { return new WebTriggerRegistrationRequest(List.copyOf(params), destination); }
    }
}
