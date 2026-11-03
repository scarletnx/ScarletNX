// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import android.os.Bundle;

/** A pending intent's sender. */
public class IntentSender implements android.os.Parcelable {
    public static class SendIntentException extends android.util.AndroidException {
        public SendIntentException() {}
        public SendIntentException(String name) { super(name); }
        public SendIntentException(Exception cause) { super(cause); }
    }
    public interface OnFinished {
        void onSendFinished(IntentSender intentSender, Intent intent, int resultCode, String resultData, Bundle resultExtras);
    }

    /** No other process takes the intent: the send finishes at once with the given code. */
    public void sendIntent(Context context, int code, Intent intent, OnFinished onFinished, android.os.Handler handler) throws SendIntentException {
        if (onFinished == null) return;
        Runnable r = () -> onFinished.onSendFinished(this, intent, code, null, null);
        if (handler != null) handler.post(r); else r.run();
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
