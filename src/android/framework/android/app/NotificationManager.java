// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** Notifications: recorded, not shown, as the host has no notification shade. */
public class NotificationManager {
    public static final int IMPORTANCE_UNSPECIFIED = -1000, IMPORTANCE_NONE = 0, IMPORTANCE_MIN = 1, IMPORTANCE_LOW = 2,
        IMPORTANCE_DEFAULT = 3, IMPORTANCE_HIGH = 4, IMPORTANCE_MAX = 5;
    private final java.util.HashMap<String, NotificationChannel> channels = new java.util.HashMap<>();

    public void notify(int id, Notification notification) {}
    public void notify(String tag, int id, Notification notification) {}
    public void cancel(int id) {}
    public void cancel(String tag, int id) {}
    public void cancelAll() {}
    public void createNotificationChannel(NotificationChannel channel) { synchronized (channels) { channels.put(channel.getId(), channel); } }
    public void createNotificationChannels(java.util.List<NotificationChannel> list) { for (NotificationChannel c : list) createNotificationChannel(c); }
    public NotificationChannel getNotificationChannel(String channelId) { synchronized (channels) { return channels.get(channelId); } }
    public java.util.List<NotificationChannel> getNotificationChannels() { synchronized (channels) { return new java.util.ArrayList<>(channels.values()); } }
    public void deleteNotificationChannel(String channelId) { synchronized (channels) { channels.remove(channelId); } }
    public boolean areNotificationsEnabled() { return true; }
    public android.service.notification.StatusBarNotification[] getActiveNotifications() { return new android.service.notification.StatusBarNotification[0]; }
}
