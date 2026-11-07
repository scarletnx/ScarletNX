// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import android.os.Bundle;

/** Takes broadcast intents: the process's own, as no other process sends any. */
public abstract class BroadcastReceiver {
    private PendingResult pendingResult;
    private boolean debugUnregister;

    public static class PendingResult {
        private int code;
        private String data;
        private Bundle extras;
        private boolean abort;

        public final void setResultCode(int code) { this.code = code; }
        public final int getResultCode() { return code; }
        public final void setResultData(String data) { this.data = data; }
        public final String getResultData() { return data; }
        public final void setResultExtras(Bundle extras) { this.extras = extras; }
        public final Bundle getResultExtras(boolean makeMap) { if (extras == null && makeMap) extras = new Bundle(); return extras; }
        public final void setResult(int code, String data, Bundle extras) { this.code = code; this.data = data; this.extras = extras; }
        public final boolean getAbortBroadcast() { return abort; }
        public final void abortBroadcast() { abort = true; }
        public final void clearAbortBroadcast() { abort = false; }
        public final void finish() {}
    }

    public BroadcastReceiver() {}
    public abstract void onReceive(Context context, Intent intent);
    public final PendingResult goAsync() { PendingResult r = pendingResult != null ? pendingResult : new PendingResult(); pendingResult = null; return r; }
    public android.os.IBinder peekService(Context myContext, Intent service) { return null; }
    public final void setResultCode(int code) {}
    public final int getResultCode() { return 0; }
    public final void setResultData(String data) {}
    public final String getResultData() { return null; }
    public final void setResultExtras(Bundle extras) {}
    public final Bundle getResultExtras(boolean makeMap) { return makeMap ? new Bundle() : null; }
    public final void setResult(int code, String data, Bundle extras) {}
    public final boolean getAbortBroadcast() { return false; }
    public final void abortBroadcast() {}
    public final void clearAbortBroadcast() {}
    public final boolean isOrderedBroadcast() { return false; }
    public final boolean isInitialStickyBroadcast() { return false; }
    public final void setOrderedHint(boolean isOrdered) {}
    public final void setPendingResult(PendingResult result) { pendingResult = result; }
    public final PendingResult getPendingResult() { return pendingResult; }
    public final void setDebugUnregister(boolean debug) { debugUnregister = debug; }
    public final boolean getDebugUnregister() { return debugUnregister; }
}
