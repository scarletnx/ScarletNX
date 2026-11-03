// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.media.AudioAttributes;
import android.net.Uri;

/** A channel of notifications. */
public final class NotificationChannel implements android.os.Parcelable {
    public static final String DEFAULT_CHANNEL_ID = "miscellaneous";
    private static final Uri DEFAULT_SOUND = Uri.parse("content://settings/system/notification_sound");
    private final String id;
    private CharSequence name;
    private String description, group, parentId, conversationId;
    private int importance, lightColor, lockscreenVisibility = NotificationManager.VISIBILITY_NO_OVERRIDE;
    private boolean showBadge = true, lights, vibration;
    private long[] vibrationPattern;
    private Uri sound = DEFAULT_SOUND;
    private AudioAttributes audioAttributes = Notification.AUDIO_ATTRIBUTES_DEFAULT;

    public NotificationChannel(String id, CharSequence name, int importance) { this.id = id; this.name = name; this.importance = importance; }
    public String getId() { return id; }
    public CharSequence getName() { return name; }
    public void setName(CharSequence name) { this.name = name; }
    public int getImportance() { return importance; }
    public void setImportance(int importance) { this.importance = importance; }
    public String getDescription() { return description; }
    public void setDescription(String description) { this.description = description; }
    public String getGroup() { return group; }
    public void setGroup(String groupId) { group = groupId; }
    public boolean canShowBadge() { return showBadge; }
    public void setShowBadge(boolean showBadge) { this.showBadge = showBadge; }
    /** No bubbles float over a desktop. */
    public boolean canBubble() { return false; }
    public void setAllowBubbles(boolean allowBubbles) {}
    /** No do-not-disturb mode exists to bypass. */
    public boolean canBypassDnd() { return false; }
    public void setBypassDnd(boolean bypassDnd) {}
    public void enableLights(boolean lights) { this.lights = lights; }
    public boolean shouldShowLights() { return lights; }
    public void setLightColor(int argb) { lightColor = argb; }
    public int getLightColor() { return lightColor; }
    public void enableVibration(boolean vibration) { this.vibration = vibration; }
    public boolean shouldVibrate() { return vibration; }
    public void setVibrationPattern(long[] vibrationPattern) {
        vibration = vibrationPattern != null && vibrationPattern.length > 0;
        this.vibrationPattern = vibrationPattern;
    }
    public long[] getVibrationPattern() { return vibrationPattern; }
    public void setSound(Uri sound, AudioAttributes audioAttributes) { this.sound = sound; this.audioAttributes = audioAttributes; }
    public Uri getSound() { return sound; }
    public AudioAttributes getAudioAttributes() { return audioAttributes; }
    public void setLockscreenVisibility(int lockscreenVisibility) { this.lockscreenVisibility = lockscreenVisibility; }
    public int getLockscreenVisibility() { return lockscreenVisibility; }
    public void setConversationId(String parentChannelId, String conversationId) { parentId = parentChannelId; this.conversationId = conversationId; }
    public String getParentChannelId() { return parentId; }
    public String getConversationId() { return conversationId; }
    /** Only the user marks a conversation important, and no user setting exists here. */
    public boolean isImportantConversation() { return false; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int f) { out.writeValue(this); }
}
