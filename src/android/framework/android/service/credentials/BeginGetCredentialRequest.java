// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.ArrayList;
import java.util.List;

/** A credential retrieval, as a provider sees it before the user picks. */
public final class BeginGetCredentialRequest implements Parcelable {
    private final List<BeginGetCredentialOption> options;
    private final CallingAppInfo callingAppInfo;

    private BeginGetCredentialRequest(List<BeginGetCredentialOption> options, CallingAppInfo info) { this.options = options; callingAppInfo = info; }
    public List<BeginGetCredentialOption> getBeginGetCredentialOptions() { return options; }
    public CallingAppInfo getCallingAppInfo() { return callingAppInfo; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<BeginGetCredentialRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public BeginGetCredentialRequest createFromParcel(Parcel in) { return (BeginGetCredentialRequest) in.readValue(null); }
        @Override public BeginGetCredentialRequest[] newArray(int size) { return new BeginGetCredentialRequest[size]; }
    };

    public static final class Builder {
        private final List<BeginGetCredentialOption> options = new ArrayList<>();
        private CallingAppInfo callingAppInfo;
        public Builder() {}
        public Builder setCallingAppInfo(CallingAppInfo callingAppInfo) { this.callingAppInfo = callingAppInfo; return this; }
        public Builder addBeginGetCredentialOption(BeginGetCredentialOption option) { options.add(option); return this; }
        public Builder setBeginGetCredentialOptions(List<BeginGetCredentialOption> getBeginCredentialOptions) { options.clear(); options.addAll(getBeginCredentialOptions); return this; }
        public BeginGetCredentialRequest build() { return new BeginGetCredentialRequest(List.copyOf(options), callingAppInfo); }
    }
}
