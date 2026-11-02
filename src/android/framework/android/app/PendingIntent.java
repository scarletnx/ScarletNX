// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.Context;
import android.content.Intent;

/** An intent to send later: an activity's start, a broadcast or a service's start, all in the process. */
public final class PendingIntent implements android.os.Parcelable {
    public static final int FLAG_ONE_SHOT = 0x40000000, FLAG_NO_CREATE = 0x20000000, FLAG_CANCEL_CURRENT = 0x10000000,
        FLAG_UPDATE_CURRENT = 0x08000000, FLAG_IMMUTABLE = 0x04000000, FLAG_MUTABLE = 0x02000000, FLAG_ALLOW_UNSAFE_IMPLICIT_INTENT = 0x01000000;
    private final Context context;
    private final Intent intent;
    private final int kind; // 1 activity, 2 broadcast, 3 service

    public static class CanceledException extends android.util.AndroidException {
        public CanceledException() {}
        public CanceledException(String name) { super(name); }
    }

    private PendingIntent(Context context, Intent intent, int kind) { this.context = context; this.intent = new Intent(intent); this.kind = kind; }
    public static PendingIntent getActivity(Context context, int requestCode, Intent intent, int flags) { return new PendingIntent(context, intent, 1); }
    public static PendingIntent getActivity(Context context, int requestCode, Intent intent, int flags, android.os.Bundle options) { return new PendingIntent(context, intent, 1); }
    public static PendingIntent getActivities(Context context, int requestCode, Intent[] intents, int flags) { return new PendingIntent(context, intents[intents.length - 1], 1); }
    public static PendingIntent getBroadcast(Context context, int requestCode, Intent intent, int flags) { return new PendingIntent(context, intent, 2); }
    public static PendingIntent getService(Context context, int requestCode, Intent intent, int flags) { return new PendingIntent(context, intent, 3); }
    public static PendingIntent getForegroundService(Context context, int requestCode, Intent intent, int flags) { return new PendingIntent(context, intent, 3); }
    public void send() throws CanceledException { sendQuietly(); }
    public void send(int code) throws CanceledException { sendQuietly(); }
    public void send(Context context, int code, Intent intent) throws CanceledException { sendQuietly(); }
    void sendQuietly() {
        switch (kind) {
            case 1 -> context.startActivity(new Intent(intent).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            case 2 -> context.sendBroadcast(intent);
            default -> context.startService(intent);
        }
    }
    public void cancel() {}
    public String getCreatorPackage() { return context.getPackageName(); }
    public String getTargetPackage() { return context.getPackageName(); }
    public int getCreatorUid() { return android.os.Process.myUid(); }
    public android.content.IntentSender getIntentSender() { return new android.content.IntentSender(); }
    public boolean isImmutable() { return true; }
    @Override public boolean equals(Object o) { return o instanceof PendingIntent p && p.kind == kind && p.intent.filterEquals(intent); }
    @Override public int hashCode() { return intent.filterHashCode() * 4 + kind; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
