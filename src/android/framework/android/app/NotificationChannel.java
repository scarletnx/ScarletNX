// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** A channel of notifications. */
public final class NotificationChannel implements android.os.Parcelable {
    private final String id;
    private CharSequence name;
    private int importance;
    public NotificationChannel(String id, CharSequence name, int importance) { this.id = id; this.name = name; this.importance = importance; }
    public String getId() { return id; }
    public CharSequence getName() { return name; }
    public int getImportance() { return importance; }
    public void setDescription(String description) {}
    public void setShowBadge(boolean showBadge) {}
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int f) { out.writeValue(this); }
}
