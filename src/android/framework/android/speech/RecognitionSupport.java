// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.speech;

import java.util.List;

/** The languages a recognizer supports on the device and online */
public final class RecognitionSupport implements android.os.Parcelable {
    private final List<String> installed, pending, supported, online;

    RecognitionSupport(List<String> installed, List<String> pending, List<String> supported, List<String> online) {
        this.installed = installed;
        this.pending = pending;
        this.supported = supported;
        this.online = online;
    }
    public List<String> getInstalledOnDeviceLanguages() { return installed; }
    public List<String> getPendingOnDeviceLanguages() { return pending; }
    public List<String> getSupportedOnDeviceLanguages() { return supported; }
    public List<String> getOnlineLanguages() { return online; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
