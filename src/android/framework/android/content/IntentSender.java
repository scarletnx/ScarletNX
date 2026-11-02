// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

/** A pending intent's sender. */
public class IntentSender implements android.os.Parcelable {
    public static class SendIntentException extends android.util.AndroidException {
        public SendIntentException() {}
        public SendIntentException(String name) { super(name); }
        public SendIntentException(Exception cause) { super(cause); }
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
