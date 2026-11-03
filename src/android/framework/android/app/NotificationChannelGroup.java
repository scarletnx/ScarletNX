// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import java.util.ArrayList;
import java.util.List;

/** A group of notification channels, as the settings of the app list them. */
public final class NotificationChannelGroup implements android.os.Parcelable {
    private final String id;
    private final CharSequence name;
    private String description;
    final ArrayList<NotificationChannel> channels = new ArrayList<>();

    public NotificationChannelGroup(String id, CharSequence name) { this.id = id; this.name = name; }
    public String getId() { return id; }
    public CharSequence getName() { return name; }
    public String getDescription() { return description; }
    public void setDescription(String description) { this.description = description; }
    /** The channels of the group, which the manager fills in when it returns the group. */
    public List<NotificationChannel> getChannels() { return channels; }
    public boolean isBlocked() { return false; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int f) { out.writeValue(this); }
}
