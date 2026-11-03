// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media.session;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.SystemClock;
import java.util.ArrayList;
import java.util.List;

/** A session's playback state: the state, the position at a time, the speed and the actions it takes. */
public final class PlaybackState implements Parcelable {
    public static final long ACTION_STOP = 1, ACTION_PAUSE = 2, ACTION_PLAY = 4, ACTION_REWIND = 8, ACTION_SKIP_TO_PREVIOUS = 16,
        ACTION_SKIP_TO_NEXT = 32, ACTION_FAST_FORWARD = 64, ACTION_SET_RATING = 128, ACTION_SEEK_TO = 256,
        ACTION_PLAY_PAUSE = 512, ACTION_PLAY_FROM_MEDIA_ID = 1024, ACTION_PLAY_FROM_SEARCH = 2048,
        ACTION_SKIP_TO_QUEUE_ITEM = 4096, ACTION_PLAY_FROM_URI = 8192, ACTION_PREPARE = 16384,
        ACTION_PREPARE_FROM_MEDIA_ID = 32768, ACTION_PREPARE_FROM_SEARCH = 65536, ACTION_PREPARE_FROM_URI = 131072,
        ACTION_SET_PLAYBACK_SPEED = 4194304;
    public static final int STATE_NONE = 0, STATE_STOPPED = 1, STATE_PAUSED = 2, STATE_PLAYING = 3, STATE_FAST_FORWARDING = 4,
        STATE_REWINDING = 5, STATE_BUFFERING = 6, STATE_ERROR = 7, STATE_CONNECTING = 8, STATE_SKIPPING_TO_PREVIOUS = 9,
        STATE_SKIPPING_TO_NEXT = 10, STATE_SKIPPING_TO_QUEUE_ITEM = 11;
    public static final long PLAYBACK_POSITION_UNKNOWN = -1;

    private final int state;
    private final long position, bufferedPosition, actions, updateTime, activeItemId;
    private final float speed;
    private final ArrayList<CustomAction> customActions;
    private final CharSequence errorMessage;
    private final Bundle extras;

    private PlaybackState(int state, long position, long updateTime, float speed, long bufferedPosition, long actions,
            List<CustomAction> customActions, long activeItemId, CharSequence error, Bundle extras) {
        this.state = state; this.position = position; this.updateTime = updateTime; this.speed = speed;
        this.bufferedPosition = bufferedPosition; this.actions = actions; this.customActions = new ArrayList<>(customActions);
        this.activeItemId = activeItemId; errorMessage = error; this.extras = extras;
    }

    public int getState() { return state; }
    public long getPosition() { return position; }
    public long getBufferedPosition() { return bufferedPosition; }
    public float getPlaybackSpeed() { return speed; }
    public long getActions() { return actions; }
    public List<CustomAction> getCustomActions() { return customActions; }
    public CharSequence getErrorMessage() { return errorMessage; }
    public long getLastPositionUpdateTime() { return updateTime; }
    public long getActiveQueueItemId() { return activeItemId; }
    public Bundle getExtras() { return extras; }
    public boolean isActive() {
        return switch (state) {
            case STATE_FAST_FORWARDING, STATE_REWINDING, STATE_SKIPPING_TO_PREVIOUS, STATE_SKIPPING_TO_NEXT,
                STATE_SKIPPING_TO_QUEUE_ITEM, STATE_BUFFERING, STATE_CONNECTING, STATE_PLAYING -> true;
            default -> false;
        };
    }

    @Override public String toString() { return "PlaybackState {state=" + state + ", position=" + position + ", speed=" + speed + "}"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<PlaybackState> CREATOR = new Parcelable.Creator<>() {
        @Override public PlaybackState createFromParcel(Parcel in) { return (PlaybackState) in.readValue(null); }
        @Override public PlaybackState[] newArray(int size) { return new PlaybackState[size]; }
    };

    public static final class CustomAction implements Parcelable {
        private final String action;
        private final CharSequence name;
        private final int icon;
        private final Bundle extras;

        private CustomAction(String action, CharSequence name, int icon, Bundle extras) {
            this.action = action; this.name = name; this.icon = icon; this.extras = extras;
        }

        public String getAction() { return action; }
        public CharSequence getName() { return name; }
        public int getIcon() { return icon; }
        public Bundle getExtras() { return extras; }

        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
        public static final Parcelable.Creator<CustomAction> CREATOR = new Parcelable.Creator<>() {
            @Override public CustomAction createFromParcel(Parcel in) { return (CustomAction) in.readValue(null); }
            @Override public CustomAction[] newArray(int size) { return new CustomAction[size]; }
        };

        public static final class Builder {
            private final String action;
            private final CharSequence name;
            private final int icon;
            private Bundle extras;

            public Builder(String action, CharSequence name, int icon) {
                if (action == null || action.isEmpty()) throw new IllegalArgumentException("You must specify an action to build a CustomAction.");
                if (name == null || name.length() == 0) throw new IllegalArgumentException("You must specify a name to build a CustomAction.");
                if (icon == 0) throw new IllegalArgumentException("You must specify an icon resource id to build a CustomAction.");
                this.action = action; this.name = name; this.icon = icon;
            }

            public Builder setExtras(Bundle extras) { this.extras = extras; return this; }
            public CustomAction build() { return new CustomAction(action, name, icon, extras); }
        }
    }

    public static final class Builder {
        private final ArrayList<CustomAction> customActions = new ArrayList<>();
        private int state;
        private long position, bufferedPosition, actions, updateTime, activeItemId = MediaSession.QueueItem.UNKNOWN_ID;
        private float speed;
        private CharSequence errorMessage;
        private Bundle extras;

        public Builder() {}
        public Builder(PlaybackState from) {
            state = from.state; position = from.position; bufferedPosition = from.bufferedPosition; speed = from.speed;
            actions = from.actions; customActions.addAll(from.customActions); errorMessage = from.errorMessage;
            updateTime = from.updateTime; activeItemId = from.activeItemId; extras = from.extras;
        }

        public Builder setState(int state, long position, float playbackSpeed, long updateTime) {
            this.state = state; this.position = position; speed = playbackSpeed; this.updateTime = updateTime;
            return this;
        }
        public Builder setState(int state, long position, float playbackSpeed) {
            return setState(state, position, playbackSpeed, SystemClock.elapsedRealtime());
        }
        public Builder setActions(long actions) { this.actions = actions; return this; }
        public Builder addCustomAction(String action, String name, int icon) { return addCustomAction(new CustomAction(action, name, icon, null)); }
        public Builder addCustomAction(CustomAction customAction) {
            if (customAction == null) throw new IllegalArgumentException("You may not add a null CustomAction to PlaybackState.");
            customActions.add(customAction);
            return this;
        }
        public Builder setBufferedPosition(long bufferedPosition) { this.bufferedPosition = bufferedPosition; return this; }
        public Builder setActiveQueueItemId(long id) { activeItemId = id; return this; }
        public Builder setErrorMessage(CharSequence error) { errorMessage = error; return this; }
        public Builder setExtras(Bundle extras) { this.extras = extras; return this; }
        public PlaybackState build() {
            return new PlaybackState(state, position, updateTime, speed, bufferedPosition, actions, customActions, activeItemId, errorMessage, extras);
        }
    }
}
