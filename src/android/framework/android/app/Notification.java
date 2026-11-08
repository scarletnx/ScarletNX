// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.app;

import android.content.Context;
import android.content.LocusId;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;
import android.media.AudioAttributes;
import android.net.Uri;
import android.os.Bundle;
import android.widget.RemoteViews;
import java.util.ArrayList;

/** A notification: kept, never shown. */
public class Notification implements android.os.Parcelable {
    public static final int FLAG_SHOW_LIGHTS = 1, FLAG_ONGOING_EVENT = 2, FLAG_INSISTENT = 4, FLAG_ONLY_ALERT_ONCE = 8,
        FLAG_AUTO_CANCEL = 16, FLAG_NO_CLEAR = 32, FLAG_FOREGROUND_SERVICE = 64, FLAG_HIGH_PRIORITY = 128, FLAG_LOCAL_ONLY = 256,
        FLAG_GROUP_SUMMARY = 512, FLAG_BUBBLE = 4096;
    public static final int DEFAULT_ALL = -1, DEFAULT_SOUND = 1, DEFAULT_VIBRATE = 2, DEFAULT_LIGHTS = 4, STREAM_DEFAULT = -1, COLOR_DEFAULT = 0;
    public static final int PRIORITY_MIN = -2, PRIORITY_LOW = -1, PRIORITY_DEFAULT = 0, PRIORITY_HIGH = 1, PRIORITY_MAX = 2;
    public static final int VISIBILITY_SECRET = -1, VISIBILITY_PRIVATE = 0, VISIBILITY_PUBLIC = 1;
    public static final int BADGE_ICON_NONE = 0, BADGE_ICON_SMALL = 1, BADGE_ICON_LARGE = 2;
    public static final int GROUP_ALERT_ALL = 0, GROUP_ALERT_SUMMARY = 1, GROUP_ALERT_CHILDREN = 2;
    public static final int FOREGROUND_SERVICE_DEFAULT = 0, FOREGROUND_SERVICE_IMMEDIATE = 1, FOREGROUND_SERVICE_DEFERRED = 2;
    public static final String CATEGORY_PROGRESS = "progress", CATEGORY_SERVICE = "service";
    public static final String EXTRA_TITLE = "android.title", EXTRA_TITLE_BIG = "android.title.big", EXTRA_TEXT = "android.text",
        EXTRA_SUB_TEXT = "android.subText", EXTRA_INFO_TEXT = "android.infoText", EXTRA_SUMMARY_TEXT = "android.summaryText",
        EXTRA_BIG_TEXT = "android.bigText", EXTRA_TEXT_LINES = "android.textLines", EXTRA_PROGRESS = "android.progress",
        EXTRA_PROGRESS_MAX = "android.progressMax", EXTRA_PROGRESS_INDETERMINATE = "android.progressIndeterminate",
        EXTRA_SHOW_WHEN = "android.showWhen", EXTRA_SHOW_CHRONOMETER = "android.showChronometer", EXTRA_COLORIZED = "android.colorized",
        EXTRA_PEOPLE_LIST = "android.people.list", EXTRA_REMOTE_INPUT_HISTORY = "android.remoteInputHistory",
        EXTRA_TEMPLATE = "android.template", EXTRA_PICTURE = "android.picture", EXTRA_PICTURE_ICON = "android.pictureIcon",
        EXTRA_PICTURE_CONTENT_DESCRIPTION = "android.pictureContentDescription",
        EXTRA_SHOW_BIG_PICTURE_WHEN_COLLAPSED = "android.showBigPictureWhenCollapsed", EXTRA_LARGE_ICON_BIG = "android.largeIcon.big",
        EXTRA_MEDIA_SESSION = "android.mediaSession", EXTRA_COMPACT_ACTIONS = "android.compactActions", EXTRA_MESSAGES = "android.messages",
        EXTRA_HISTORIC_MESSAGES = "android.messages.historic", EXTRA_CONVERSATION_TITLE = "android.conversationTitle",
        EXTRA_IS_GROUP_CONVERSATION = "android.isGroupConversation", EXTRA_MESSAGING_PERSON = "android.messagingUser";
    public static final AudioAttributes AUDIO_ATTRIBUTES_DEFAULT = new AudioAttributes.Builder()
        .setContentType(4).setUsage(5).build(); // CONTENT_TYPE_SONIFICATION, USAGE_NOTIFICATION

    public int flags, icon, iconLevel, number, defaults, color = COLOR_DEFAULT, priority = PRIORITY_DEFAULT, visibility = VISIBILITY_PRIVATE;
    public int audioStreamType = STREAM_DEFAULT, ledARGB, ledOnMS, ledOffMS;
    public long when = System.currentTimeMillis();
    public long[] vibrate;
    public CharSequence tickerText;
    public String category;
    public PendingIntent contentIntent, deleteIntent, fullScreenIntent;
    public RemoteViews contentView, bigContentView, headsUpContentView;
    public Bitmap largeIcon;
    public Uri sound;
    public AudioAttributes audioAttributes = AUDIO_ATTRIBUTES_DEFAULT;
    public Notification publicVersion;
    public Action[] actions;
    public Bundle extras = new Bundle();
    private String channelId, group, sortKey, shortcutId;
    private CharSequence settingsText;
    private LocusId locusId;
    private BubbleMetadata bubbleMetadata;
    private Icon smallIcon;
    private long timeoutAfter;
    private int badgeIconType = BADGE_ICON_NONE, groupAlertBehavior = GROUP_ALERT_ALL;
    private boolean allowSystemGeneratedContextualActions = true;

    public Notification() {}
    public String getChannelId() { return channelId; }
    public String getGroup() { return group; }
    public String getSortKey() { return sortKey; }
    public String getShortcutId() { return shortcutId; }
    public CharSequence getSettingsText() { return settingsText; }
    public LocusId getLocusId() { return locusId; }
    public BubbleMetadata getBubbleMetadata() { return bubbleMetadata; }
    public Icon getSmallIcon() { return smallIcon; }
    public long getTimeoutAfter() { return timeoutAfter; }
    public int getBadgeIconType() { return badgeIconType; }
    public int getGroupAlertBehavior() { return groupAlertBehavior; }
    public boolean getAllowSystemGeneratedContextualActions() { return allowSystemGeneratedContextualActions; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int f) { out.writeValue(this); }
    public static final Creator<Notification> CREATOR = new Creator<>() {
        @Override public Notification createFromParcel(android.os.Parcel in) { return (Notification) in.readValue(null); }
        @Override public Notification[] newArray(int size) { return new Notification[size]; }
    };

    public static class Action implements android.os.Parcelable {
        public static final int SEMANTIC_ACTION_NONE = 0, SEMANTIC_ACTION_REPLY = 1, SEMANTIC_ACTION_MARK_AS_READ = 2,
            SEMANTIC_ACTION_MARK_AS_UNREAD = 3, SEMANTIC_ACTION_DELETE = 4, SEMANTIC_ACTION_ARCHIVE = 5, SEMANTIC_ACTION_MUTE = 6,
            SEMANTIC_ACTION_UNMUTE = 7, SEMANTIC_ACTION_THUMBS_UP = 8, SEMANTIC_ACTION_THUMBS_DOWN = 9, SEMANTIC_ACTION_CALL = 10;
        @Deprecated public int icon;
        public CharSequence title;
        public PendingIntent actionIntent;
        private Icon iconObject;
        private Bundle extras;
        private RemoteInput[] remoteInputs;
        private boolean allowGeneratedReplies = true, contextual, authenticationRequired;
        private int semanticAction;

        @Deprecated public Action(int icon, CharSequence title, PendingIntent intent) {
            this(icon != 0 ? Icon.createWithResource("", icon) : null, title, intent);
            this.icon = icon;
        }
        private Action(Icon icon, CharSequence title, PendingIntent intent) {
            iconObject = icon;
            this.title = title;
            actionIntent = intent;
            extras = new Bundle();
        }
        public Icon getIcon() { return iconObject; }
        public Bundle getExtras() { return extras; }
        public RemoteInput[] getRemoteInputs() { return remoteInputs; }
        public boolean getAllowGeneratedReplies() { return allowGeneratedReplies; }
        public int getSemanticAction() { return semanticAction; }
        public boolean isContextual() { return contextual; }
        public boolean isAuthenticationRequired() { return authenticationRequired; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel out, int f) { out.writeValue(this); }

        public static final class Builder {
            private final Icon icon;
            private final CharSequence title;
            private final PendingIntent intent;
            private final Bundle extras = new Bundle();
            private final ArrayList<RemoteInput> remoteInputs = new ArrayList<>();
            private boolean allowGeneratedReplies = true, contextual, authenticationRequired;
            private int semanticAction;

            public Builder(Icon icon, CharSequence title, PendingIntent intent) { this.icon = icon; this.title = title; this.intent = intent; }
            @Deprecated public Builder(int icon, CharSequence title, PendingIntent intent) {
                this(icon != 0 ? Icon.createWithResource("", icon) : null, title, intent);
            }
            public Builder(Action action) {
                this(action.iconObject, action.title, action.actionIntent);
                extras.putAll(action.extras);
                if (action.remoteInputs != null) for (RemoteInput r : action.remoteInputs) remoteInputs.add(r);
                allowGeneratedReplies = action.allowGeneratedReplies;
                semanticAction = action.semanticAction;
                contextual = action.contextual;
                authenticationRequired = action.authenticationRequired;
            }
            public Builder addExtras(Bundle extras) { if (extras != null) this.extras.putAll(extras); return this; }
            public Bundle getExtras() { return extras; }
            public Builder addRemoteInput(RemoteInput remoteInput) { remoteInputs.add(remoteInput); return this; }
            public Builder setAllowGeneratedReplies(boolean allowGeneratedReplies) { this.allowGeneratedReplies = allowGeneratedReplies; return this; }
            public Builder setSemanticAction(int semanticAction) { this.semanticAction = semanticAction; return this; }
            public Builder setContextual(boolean isContextual) { contextual = isContextual; return this; }
            public Builder setAuthenticationRequired(boolean authenticationRequired) { this.authenticationRequired = authenticationRequired; return this; }
            public Action build() {
                Action a = new Action(icon, title, intent);
                if (icon != null && icon.getType() == Icon.TYPE_RESOURCE) a.icon = icon.getResId();
                a.extras.putAll(extras);
                a.remoteInputs = remoteInputs.isEmpty() ? null : remoteInputs.toArray(new RemoteInput[0]);
                a.allowGeneratedReplies = allowGeneratedReplies;
                a.semanticAction = semanticAction;
                a.contextual = contextual;
                a.authenticationRequired = authenticationRequired;
                return a;
            }
        }
    }

    public static final class BubbleMetadata implements android.os.Parcelable {
        private PendingIntent intent, deleteIntent;
        private Icon icon;
        private String shortcutId;
        private int desiredHeight, desiredHeightResId;
        private boolean autoExpand, suppressNotification;

        private BubbleMetadata() {}
        public PendingIntent getIntent() { return intent; }
        public PendingIntent getDeleteIntent() { return deleteIntent; }
        public Icon getIcon() { return icon; }
        public String getShortcutId() { return shortcutId; }
        public int getDesiredHeight() { return desiredHeight; }
        public int getDesiredHeightResId() { return desiredHeightResId; }
        public boolean getAutoExpandBubble() { return autoExpand; }
        public boolean isNotificationSuppressed() { return suppressNotification; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel out, int f) { out.writeValue(this); }

        public static final class Builder {
            private final BubbleMetadata m = new BubbleMetadata();

            @Deprecated public Builder() {}
            public Builder(String shortcutId) { m.shortcutId = shortcutId; }
            public Builder(PendingIntent intent, Icon icon) { m.intent = intent; m.icon = icon; }
            public Builder setIntent(PendingIntent intent) { m.intent = intent; return this; }
            public Builder setIcon(Icon icon) { m.icon = icon; return this; }
            public Builder setDeleteIntent(PendingIntent deleteIntent) { m.deleteIntent = deleteIntent; return this; }
            public Builder setDesiredHeight(int height) { m.desiredHeight = Math.max(height, 0); m.desiredHeightResId = 0; return this; }
            public Builder setDesiredHeightResId(int heightResId) { m.desiredHeightResId = heightResId; m.desiredHeight = 0; return this; }
            public Builder setAutoExpandBubble(boolean shouldExpand) { m.autoExpand = shouldExpand; return this; }
            public Builder setSuppressNotification(boolean shouldSuppressNotif) { m.suppressNotification = shouldSuppressNotif; return this; }
            public BubbleMetadata build() {
                BubbleMetadata b = new BubbleMetadata();
                b.intent = m.intent; b.deleteIntent = m.deleteIntent; b.icon = m.icon; b.shortcutId = m.shortcutId;
                b.desiredHeight = m.desiredHeight; b.desiredHeightResId = m.desiredHeightResId;
                b.autoExpand = m.autoExpand; b.suppressNotification = m.suppressNotification;
                return b;
            }
        }
    }

    public static class Builder {
        private final Context context;
        private final Notification n;
        private final ArrayList<Action> actions = new ArrayList<>();
        private final ArrayList<Person> people = new ArrayList<>();
        private Style style;

        public Builder(Context context, String channelId) { this(context, new Notification()); n.channelId = channelId; }
        @Deprecated public Builder(Context context) { this(context, (String) null); }
        private Builder(Context context, Notification n) {
            this.context = context;
            this.n = n;
            if (n.actions != null) for (Action a : n.actions) actions.add(a);
        }
        /** A builder over a notification that the app built before. */
        public static Builder recoverBuilder(Context context, Notification n) { return new Builder(context, n); }

        public Bundle getExtras() { return n.extras; }
        public Builder setExtras(Bundle extras) { if (extras != null) n.extras.putAll(extras); return this; }
        public Builder addExtras(Bundle extras) { return setExtras(extras); }
        public Builder setChannelId(String channelId) { n.channelId = channelId; return this; }
        public Builder setSmallIcon(int icon) { return setSmallIcon(icon != 0 ? Icon.createWithResource(context, icon) : null); }
        public Builder setSmallIcon(int icon, int level) { n.iconLevel = level; return setSmallIcon(icon); }
        public Builder setSmallIcon(Icon icon) {
            n.smallIcon = icon;
            n.icon = icon != null && icon.getType() == Icon.TYPE_RESOURCE ? icon.getResId() : 0;
            return this;
        }
        public Builder setLargeIcon(Bitmap b) { n.largeIcon = b; return this; }
        public Builder setLargeIcon(Icon icon) { return this; }
        public Builder setContentTitle(CharSequence title) { n.extras.putCharSequence(EXTRA_TITLE, title); return this; }
        public Builder setContentText(CharSequence text) { n.extras.putCharSequence(EXTRA_TEXT, text); return this; }
        public Builder setSubText(CharSequence text) { n.extras.putCharSequence(EXTRA_SUB_TEXT, text); return this; }
        @Deprecated public Builder setContentInfo(CharSequence info) { n.extras.putCharSequence(EXTRA_INFO_TEXT, info); return this; }
        public Builder setSettingsText(CharSequence text) { n.settingsText = text; return this; }
        public Builder setTicker(CharSequence ticker) { n.tickerText = ticker; return this; }
        @Deprecated public Builder setTicker(CharSequence tickerText, RemoteViews views) { return setTicker(tickerText); }
        public Builder setOngoing(boolean ongoing) { return flag(FLAG_ONGOING_EVENT, ongoing); }
        public Builder setAutoCancel(boolean autoCancel) { return flag(FLAG_AUTO_CANCEL, autoCancel); }
        public Builder setOnlyAlertOnce(boolean onlyAlertOnce) { return flag(FLAG_ONLY_ALERT_ONCE, onlyAlertOnce); }
        public Builder setLocalOnly(boolean localOnly) { return flag(FLAG_LOCAL_ONLY, localOnly); }
        public Builder setGroupSummary(boolean isGroupSummary) { return flag(FLAG_GROUP_SUMMARY, isGroupSummary); }
        private Builder flag(int mask, boolean value) { n.flags = value ? n.flags | mask : n.flags & ~mask; return this; }
        public Builder setContentIntent(PendingIntent intent) { n.contentIntent = intent; return this; }
        public Builder setDeleteIntent(PendingIntent intent) { n.deleteIntent = intent; return this; }
        public Builder setFullScreenIntent(PendingIntent intent, boolean highPriority) { n.fullScreenIntent = intent; return flag(FLAG_HIGH_PRIORITY, highPriority); }
        public Builder setProgress(int max, int progress, boolean indeterminate) {
            n.extras.putInt(EXTRA_PROGRESS, progress);
            n.extras.putInt(EXTRA_PROGRESS_MAX, max);
            n.extras.putBoolean(EXTRA_PROGRESS_INDETERMINATE, indeterminate);
            return this;
        }
        public Builder setCategory(String category) { n.category = category; return this; }
        @Deprecated public Builder addAction(int icon, CharSequence title, PendingIntent intent) { return addAction(new Action(icon, title, intent)); }
        public Builder addAction(Action action) { if (action != null) actions.add(action); return this; }
        public Builder addPerson(Person person) { if (person != null) people.add(person); return this; }
        @Deprecated public Builder addPerson(String uri) { return addPerson(new Person.Builder().setUri(uri).build()); }
        public Builder setStyle(Style style) {
            if (this.style != style) {
                this.style = style;
                if (style != null) style.setBuilder(this);
            }
            return this;
        }
        public Builder setWhen(long when) { n.when = when; return this; }
        public Builder setShowWhen(boolean show) { n.extras.putBoolean(EXTRA_SHOW_WHEN, show); return this; }
        public Builder setUsesChronometer(boolean b) { n.extras.putBoolean(EXTRA_SHOW_CHRONOMETER, b); return this; }
        public Builder setNumber(int number) { n.number = number; return this; }
        public Builder setColor(int argb) { n.color = argb; return this; }
        public Builder setColorized(boolean colorize) { n.extras.putBoolean(EXTRA_COLORIZED, colorize); return this; }
        @Deprecated public Builder setDefaults(int defaults) { n.defaults = defaults; return this; }
        @Deprecated public Builder setPriority(int pri) { n.priority = pri; return this; }
        public Builder setVisibility(int visibility) { n.visibility = visibility; return this; }
        public Builder setPublicVersion(Notification n) { this.n.publicVersion = n; return this; }
        @Deprecated public Builder setSound(Uri sound) { n.sound = sound; n.audioAttributes = AUDIO_ATTRIBUTES_DEFAULT; return this; }
        @Deprecated public Builder setSound(Uri sound, AudioAttributes audioAttributes) { n.sound = sound; n.audioAttributes = audioAttributes; return this; }
        @Deprecated public Builder setVibrate(long[] pattern) { n.vibrate = pattern; return this; }
        @Deprecated public Builder setLights(int argb, int onMs, int offMs) {
            n.ledARGB = argb;
            n.ledOnMS = onMs;
            n.ledOffMS = offMs;
            return flag(FLAG_SHOW_LIGHTS, argb != 0 || onMs != 0 || offMs != 0);
        }
        public Builder setGroup(String groupKey) { n.group = groupKey; return this; }
        public Builder setGroupAlertBehavior(int groupAlertBehavior) { n.groupAlertBehavior = groupAlertBehavior; return this; }
        public Builder setSortKey(String sortKey) { n.sortKey = sortKey; return this; }
        public Builder setShortcutId(String shortcutId) { n.shortcutId = shortcutId; return this; }
        public Builder setLocusId(LocusId locusId) { n.locusId = locusId; return this; }
        public Builder setBubbleMetadata(BubbleMetadata data) { n.bubbleMetadata = data; return this; }
        public Builder setBadgeIconType(int icon) { n.badgeIconType = icon; return this; }
        public Builder setTimeoutAfter(long durationMs) { n.timeoutAfter = durationMs; return this; }
        public Builder setForegroundServiceBehavior(int behavior) { return this; }
        public Builder setAllowSystemGeneratedContextualActions(boolean allowed) { n.allowSystemGeneratedContextualActions = allowed; return this; }
        public Builder setRemoteInputHistory(CharSequence[] text) { n.extras.putCharSequenceArray(EXTRA_REMOTE_INPUT_HISTORY, text); return this; }
        @Deprecated public Builder setContent(RemoteViews views) { return setCustomContentView(views); }
        public Builder setCustomContentView(RemoteViews contentView) { n.contentView = contentView; return this; }
        public Builder setCustomBigContentView(RemoteViews contentView) { n.bigContentView = contentView; return this; }
        public Builder setCustomHeadsUpContentView(RemoteViews contentView) { n.headsUpContentView = contentView; return this; }
        /** The app's own views, or null: no shade draws the standard templates. */
        public RemoteViews createContentView() { return n.contentView; }
        public RemoteViews createBigContentView() { return n.bigContentView; }
        public RemoteViews createHeadsUpContentView() { return n.headsUpContentView; }
        public Notification build() {
            n.actions = actions.isEmpty() ? null : actions.toArray(new Action[0]);
            if (!people.isEmpty()) n.extras.putParcelableArrayList(EXTRA_PEOPLE_LIST, new ArrayList<>(people));
            if (style != null) {
                n.extras.putString(EXTRA_TEMPLATE, style.getClass().getName());
                style.addExtras(n.extras);
            }
            return n;
        }
        @Deprecated public Notification getNotification() { return build(); }
    }

    public abstract static class Style {
        protected Builder mBuilder;
        private CharSequence bigContentTitle, summaryText;

        public Style() {}
        protected void internalSetBigContentTitle(CharSequence title) { bigContentTitle = title; }
        protected void internalSetSummaryText(CharSequence cs) { summaryText = cs; }
        public void setBuilder(Builder builder) {
            if (mBuilder != builder) {
                mBuilder = builder;
                if (builder != null) builder.setStyle(this);
            }
        }
        protected void checkBuilder() { if (mBuilder == null) throw new IllegalArgumentException("Style requires a valid Builder object"); }
        public Notification build() { checkBuilder(); return mBuilder.build(); }
        void addExtras(Bundle extras) {
            if (bigContentTitle != null) extras.putCharSequence(EXTRA_TITLE_BIG, bigContentTitle);
            if (summaryText != null) extras.putCharSequence(EXTRA_SUMMARY_TEXT, summaryText);
        }
    }

    public static class BigTextStyle extends Style {
        private CharSequence bigText;
        public BigTextStyle() {}
        @Deprecated public BigTextStyle(Builder builder) { setBuilder(builder); }
        public BigTextStyle setBigContentTitle(CharSequence title) { internalSetBigContentTitle(title); return this; }
        public BigTextStyle setSummaryText(CharSequence cs) { internalSetSummaryText(cs); return this; }
        public BigTextStyle bigText(CharSequence cs) { bigText = cs; return this; }
        @Override void addExtras(Bundle extras) { super.addExtras(extras); extras.putCharSequence(EXTRA_BIG_TEXT, bigText); }
    }

    public static class BigPictureStyle extends Style {
        private Icon picture;
        private CharSequence contentDescription;
        private boolean showWhenCollapsed;
        public BigPictureStyle() {}
        @Deprecated public BigPictureStyle(Builder builder) { setBuilder(builder); }
        public BigPictureStyle setBigContentTitle(CharSequence title) { internalSetBigContentTitle(title); return this; }
        public BigPictureStyle setSummaryText(CharSequence cs) { internalSetSummaryText(cs); return this; }
        public BigPictureStyle setContentDescription(CharSequence contentDescription) { this.contentDescription = contentDescription; return this; }
        public BigPictureStyle bigPicture(Bitmap b) { picture = b != null ? Icon.createWithBitmap(b) : null; return this; }
        public BigPictureStyle bigPicture(Icon icon) { picture = icon; return this; }
        public BigPictureStyle bigLargeIcon(Bitmap b) { return this; }
        public BigPictureStyle bigLargeIcon(Icon icon) { return this; }
        public BigPictureStyle showBigPictureWhenCollapsed(boolean show) { showWhenCollapsed = show; return this; }
        @Override void addExtras(Bundle extras) {
            super.addExtras(extras);
            extras.putParcelable(EXTRA_PICTURE_ICON, picture);
            extras.putCharSequence(EXTRA_PICTURE_CONTENT_DESCRIPTION, contentDescription);
            extras.putBoolean(EXTRA_SHOW_BIG_PICTURE_WHEN_COLLAPSED, showWhenCollapsed);
        }
    }

    public static class InboxStyle extends Style {
        private final ArrayList<CharSequence> lines = new ArrayList<>();
        public InboxStyle() {}
        @Deprecated public InboxStyle(Builder builder) { setBuilder(builder); }
        public InboxStyle setBigContentTitle(CharSequence title) { internalSetBigContentTitle(title); return this; }
        public InboxStyle setSummaryText(CharSequence cs) { internalSetSummaryText(cs); return this; }
        public InboxStyle addLine(CharSequence cs) { lines.add(cs); return this; }
        @Override void addExtras(Bundle extras) { super.addExtras(extras); extras.putCharSequenceArray(EXTRA_TEXT_LINES, lines.toArray(new CharSequence[0])); }
    }

    public static class MediaStyle extends Style {
        private android.media.session.MediaSession.Token token;
        private int[] compactActions;
        public MediaStyle() {}
        @Deprecated public MediaStyle(Builder builder) { setBuilder(builder); }
        public MediaStyle setMediaSession(android.media.session.MediaSession.Token token) { this.token = token; return this; }
        public MediaStyle setShowActionsInCompactView(int... actions) { compactActions = actions; return this; }
        @Override void addExtras(Bundle extras) {
            super.addExtras(extras);
            if (token != null) extras.putParcelable(EXTRA_MEDIA_SESSION, token);
            if (compactActions != null) extras.putIntArray(EXTRA_COMPACT_ACTIONS, compactActions);
        }
    }

    public static class DecoratedCustomViewStyle extends Style {
        public DecoratedCustomViewStyle() {}
    }

    public static class DecoratedMediaCustomViewStyle extends MediaStyle {
        public DecoratedMediaCustomViewStyle() {}
    }

    public static class MessagingStyle extends Style {
        public static final int MAXIMUM_RETAINED_MESSAGES = 25;
        private final Person user;
        private final ArrayList<Message> messages = new ArrayList<>(), historicMessages = new ArrayList<>();
        private CharSequence conversationTitle;
        private boolean groupConversation;

        @Deprecated public MessagingStyle(CharSequence userDisplayName) { this(new Person.Builder().setName(userDisplayName).build()); }
        public MessagingStyle(Person user) { this.user = user; }
        public Person getUser() { return user; }
        @Deprecated public CharSequence getUserDisplayName() { return user.getName(); }
        public MessagingStyle setConversationTitle(CharSequence conversationTitle) { this.conversationTitle = conversationTitle; return this; }
        public CharSequence getConversationTitle() { return conversationTitle; }
        public MessagingStyle setGroupConversation(boolean isGroupConversation) { groupConversation = isGroupConversation; return this; }
        public boolean isGroupConversation() { return groupConversation; }
        public MessagingStyle addMessage(CharSequence text, long timestamp, Person sender) { return addMessage(new Message(text, timestamp, sender)); }
        public MessagingStyle addMessage(Message message) { return add(messages, message); }
        public MessagingStyle addHistoricMessage(Message message) { return add(historicMessages, message); }
        public java.util.List<Message> getMessages() { return messages; }
        public java.util.List<Message> getHistoricMessages() { return historicMessages; }
        private MessagingStyle add(ArrayList<Message> list, Message m) {
            list.add(m);
            if (list.size() > MAXIMUM_RETAINED_MESSAGES) list.remove(0);
            return this;
        }
        @Override void addExtras(Bundle extras) {
            super.addExtras(extras);
            extras.putParcelable(EXTRA_MESSAGING_PERSON, user);
            if (conversationTitle != null) extras.putCharSequence(EXTRA_CONVERSATION_TITLE, conversationTitle);
            extras.putBoolean(EXTRA_IS_GROUP_CONVERSATION, groupConversation);
        }

        public static final class Message {
            private final CharSequence text;
            private final long timestamp;
            private final Person sender;
            private final Bundle extras = new Bundle();
            private String dataMimeType;
            private Uri dataUri;

            public Message(CharSequence text, long timestamp, Person sender) { this.text = text; this.timestamp = timestamp; this.sender = sender; }
            @Deprecated public Message(CharSequence text, long timestamp, CharSequence sender) {
                this(text, timestamp, sender != null ? new Person.Builder().setName(sender).build() : null);
            }
            public Message setData(String dataMimeType, Uri dataUri) { this.dataMimeType = dataMimeType; this.dataUri = dataUri; return this; }
            public CharSequence getText() { return text; }
            public long getTimestamp() { return timestamp; }
            public Person getSenderPerson() { return sender; }
            @Deprecated public CharSequence getSender() { return sender != null ? sender.getName() : null; }
            public String getDataMimeType() { return dataMimeType; }
            public Uri getDataUri() { return dataUri; }
            public Bundle getExtras() { return extras; }
        }
    }
}
