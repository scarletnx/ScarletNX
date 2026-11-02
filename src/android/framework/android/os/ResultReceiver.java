// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A callback for a result. */
public class ResultReceiver implements Parcelable {
    private final Handler handler;
    public ResultReceiver(Handler handler) { this.handler = handler; }
    public void send(int resultCode, Bundle resultData) {
        if (handler != null) handler.post(() -> onReceiveResult(resultCode, resultData));
        else onReceiveResult(resultCode, resultData);
    }
    protected void onReceiveResult(int resultCode, Bundle resultData) {}
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) { out.writeValue(this); }
}
