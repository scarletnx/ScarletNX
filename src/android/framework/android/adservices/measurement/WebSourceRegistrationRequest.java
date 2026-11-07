// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.measurement;

import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;
import android.view.InputEvent;
import java.util.List;

/** A request to register attribution sources from the web. */
public final class WebSourceRegistrationRequest implements Parcelable {
    private final List<WebSourceParams> params;
    private final Uri topOriginUri, verifiedDestination, appDestination, webDestination;
    private final InputEvent inputEvent;

    private WebSourceRegistrationRequest(Builder b) {
        params = List.copyOf(b.params); topOriginUri = b.topOriginUri; verifiedDestination = b.verifiedDestination;
        appDestination = b.appDestination; webDestination = b.webDestination; inputEvent = b.inputEvent;
    }
    public List<WebSourceParams> getSourceParams() { return params; }
    public Uri getTopOriginUri() { return topOriginUri; }
    public InputEvent getInputEvent() { return inputEvent; }
    public Uri getVerifiedDestination() { return verifiedDestination; }
    public Uri getAppDestination() { return appDestination; }
    public Uri getWebDestination() { return webDestination; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<WebSourceRegistrationRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public WebSourceRegistrationRequest createFromParcel(Parcel in) { return (WebSourceRegistrationRequest) in.readValue(null); }
        @Override public WebSourceRegistrationRequest[] newArray(int size) { return new WebSourceRegistrationRequest[size]; }
    };

    public static final class Builder {
        private final List<WebSourceParams> params;
        private final Uri topOriginUri;
        private Uri verifiedDestination, appDestination, webDestination;
        private InputEvent inputEvent;

        public Builder(List<WebSourceParams> webSourceParams, Uri topOriginUri) { params = webSourceParams; this.topOriginUri = topOriginUri; }
        public Builder setInputEvent(InputEvent inputEvent) { this.inputEvent = inputEvent; return this; }
        public Builder setVerifiedDestination(Uri verifiedDestination) { this.verifiedDestination = verifiedDestination; return this; }
        public Builder setAppDestination(Uri appDestination) { this.appDestination = appDestination; return this; }
        public Builder setWebDestination(Uri webDestination) { this.webDestination = webDestination; return this; }
        public WebSourceRegistrationRequest build() { return new WebSourceRegistrationRequest(this); }
    }
}
