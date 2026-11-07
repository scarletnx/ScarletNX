// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

/** Notifications: recorded, not shown, as the host has no notification shade. */
public class NotificationManager {
    public static final int IMPORTANCE_UNSPECIFIED = -1000, IMPORTANCE_NONE = 0, IMPORTANCE_MIN = 1, IMPORTANCE_LOW = 2,
        IMPORTANCE_DEFAULT = 3, IMPORTANCE_HIGH = 4, IMPORTANCE_MAX = 5;
    public static final int VISIBILITY_NO_OVERRIDE = -1000;
    private final HashMap<String, NotificationChannel> channels = new HashMap<>();
    private final HashMap<String, NotificationChannelGroup> groups = new HashMap<>();
    private String delegate;

    public void notify(int id, Notification notification) {}
    public void notify(String tag, int id, Notification notification) {}
    public void cancel(int id) {}
    public void cancel(String tag, int id) {}
    public void cancelAll() {}
    public void createNotificationChannel(NotificationChannel channel) { synchronized (channels) { channels.put(channel.getId(), channel); } }
    public void createNotificationChannels(List<NotificationChannel> list) { for (NotificationChannel c : list) createNotificationChannel(c); }
    public NotificationChannel getNotificationChannel(String channelId) { synchronized (channels) { return channels.get(channelId); } }
    /** The channel of the conversation, else its parent channel. */
    public NotificationChannel getNotificationChannel(String channelId, String conversationId) {
        synchronized (channels) {
            for (NotificationChannel c : channels.values())
                if (channelId.equals(c.getParentChannelId()) && conversationId != null && conversationId.equals(c.getConversationId())) return c;
            return channels.get(channelId);
        }
    }
    public List<NotificationChannel> getNotificationChannels() { synchronized (channels) { return new ArrayList<>(channels.values()); } }
    public void deleteNotificationChannel(String channelId) { synchronized (channels) { channels.remove(channelId); } }
    public void createNotificationChannelGroup(NotificationChannelGroup group) { synchronized (channels) { groups.put(group.getId(), group); } }
    public void createNotificationChannelGroups(List<NotificationChannelGroup> list) { for (NotificationChannelGroup g : list) createNotificationChannelGroup(g); }
    /** Deletes the group and each channel in it. */
    public void deleteNotificationChannelGroup(String groupId) {
        synchronized (channels) {
            groups.remove(groupId);
            channels.values().removeIf(c -> groupId.equals(c.getGroup()));
        }
    }
    public NotificationChannelGroup getNotificationChannelGroup(String channelGroupId) {
        synchronized (channels) { return filled(groups.get(channelGroupId)); }
    }
    public List<NotificationChannelGroup> getNotificationChannelGroups() {
        synchronized (channels) {
            ArrayList<NotificationChannelGroup> list = new ArrayList<>();
            for (NotificationChannelGroup g : groups.values()) list.add(filled(g));
            return list;
        }
    }
    private NotificationChannelGroup filled(NotificationChannelGroup g) {
        if (g == null) return null;
        NotificationChannelGroup copy = new NotificationChannelGroup(g.getId(), g.getName());
        copy.setDescription(g.getDescription());
        for (NotificationChannel c : channels.values()) if (g.getId().equals(c.getGroup())) copy.channels.add(c);
        return copy;
    }
    public boolean areNotificationsEnabled() { return true; }
    /** The user set no importance for the app. */
    public int getImportance() { return IMPORTANCE_UNSPECIFIED; }
    public void setNotificationDelegate(String delegate) { this.delegate = delegate; }
    public String getNotificationDelegate() { return delegate; }
    public android.service.notification.StatusBarNotification[] getActiveNotifications() { return new android.service.notification.StatusBarNotification[0]; }
}
