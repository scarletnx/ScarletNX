// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** A notification: kept, never shown. */
public class Notification implements android.os.Parcelable {
    public static final int FLAG_ONGOING_EVENT = 2, FLAG_FOREGROUND_SERVICE = 64;
    public static final String CATEGORY_PROGRESS = "progress", CATEGORY_SERVICE = "service";
    public int flags, icon;
    public long when;
    public CharSequence tickerText;
    public PendingIntent contentIntent;
    public android.os.Bundle extras = new android.os.Bundle();

    public Notification() {}
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int f) { out.writeValue(this); }

    public static class Builder {
        public Builder(android.content.Context context, String channelId) {}
        public Builder(android.content.Context context) {}
        public Builder setSmallIcon(int icon) { return this; }
        public Builder setContentTitle(CharSequence title) { return this; }
        public Builder setContentText(CharSequence text) { return this; }
        public Builder setTicker(CharSequence ticker) { return this; }
        public Builder setOngoing(boolean ongoing) { return this; }
        public Builder setContentIntent(PendingIntent intent) { return this; }
        public Builder setProgress(int max, int progress, boolean indeterminate) { return this; }
        public Builder setCategory(String category) { return this; }
        public Builder addAction(int icon, CharSequence title, PendingIntent intent) { return this; }
        public Builder setAutoCancel(boolean autoCancel) { return this; }
        public Notification build() { return new Notification(); }
    }
}
