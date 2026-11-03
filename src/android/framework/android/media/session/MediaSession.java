// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media.session;

import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.MediaDescription;
import android.media.MediaMetadata;
import android.media.Rating;
import android.media.VolumeProvider;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.Process;
import android.os.ResultReceiver;
import android.view.KeyEvent;
import java.util.ArrayList;
import java.util.List;

/**
 * A media session. Its controllers are in the process: a controller made from the session's token reads the
 * session's state and calls its callback on the callback's handler. No system UI or other app controls it.
 */
public final class MediaSession {
    public static final int FLAG_HANDLES_MEDIA_BUTTONS = 1, FLAG_HANDLES_TRANSPORT_CONTROLS = 2;
    static final String ACTION_MEDIA_BUTTON = "android.intent.action.MEDIA_BUTTON", EXTRA_KEY_EVENT = "android.intent.extra.KEY_EVENT";

    private final Context context;
    final String tag;
    final Token token = new Token(this);
    private Callback callback;
    private Handler callbackHandler;
    private boolean active, released;
    final ArrayList<Object[]> controllerCallbacks = new ArrayList<>();
    MediaMetadata metadata;
    PlaybackState state;
    List<QueueItem> queue;
    CharSequence queueTitle;
    Bundle extras;
    long flags;
    int ratingType;
    PendingIntent sessionActivity;
    AudioAttributes localAttributes = new AudioAttributes.Builder().setLegacyStreamType(android.media.AudioManager.STREAM_MUSIC).build();
    VolumeProvider volumeProvider;

    public MediaSession(Context context, String tag) {
        if (context == null) throw new IllegalArgumentException("context cannot be null.");
        if (tag == null || tag.isEmpty()) throw new IllegalArgumentException("tag cannot be null or empty");
        this.context = context;
        this.tag = tag;
    }

    public MediaSession(Context context, String tag, Bundle sessionInfo) { this(context, tag); }

    String packageName() { return context.getPackageName(); }

    public void setCallback(Callback callback) { setCallback(callback, null); }

    public void setCallback(Callback callback, Handler handler) {
        if (this.callback != null) this.callback.session = null;
        this.callback = callback;
        if (callback != null) callback.session = this;
        Looper looper = Looper.myLooper();
        callbackHandler = handler != null ? handler : new Handler(looper != null ? looper : Looper.getMainLooper());
    }

    /** Runs code on the callback's handler, if the session has a callback. */
    void dispatch(java.util.function.Consumer<Callback> call) {
        Callback cb = callback;
        if (cb == null || released) return;
        callbackHandler.post(() -> { if (callback == cb) call.accept(cb); });
    }

    /** Each controller callback hears the change, on its own handler. */
    void notifyControllers(java.util.function.Consumer<MediaController.Callback> call) {
        List<Object[]> snapshot;
        synchronized (controllerCallbacks) { snapshot = new ArrayList<>(controllerCallbacks); }
        for (Object[] e : snapshot) {
            MediaController.Callback cb = (MediaController.Callback) e[0];
            ((Handler) e[1]).post(() -> call.accept(cb));
        }
    }

    public void setSessionActivity(PendingIntent pi) { sessionActivity = pi; }
    public void setMediaButtonReceiver(PendingIntent mbr) {}
    public void setMediaButtonBroadcastReceiver(ComponentName broadcastReceiver) {}
    public void setFlags(int flags) { this.flags = flags; }

    public void setPlaybackToLocal(AudioAttributes attributes) {
        if (attributes == null) throw new IllegalArgumentException("Attributes cannot be null for local playback.");
        localAttributes = attributes;
        volumeProvider = null;
        notifyControllers(cb -> cb.onAudioInfoChanged(MediaController.playbackInfo(this)));
    }

    public void setPlaybackToRemote(VolumeProvider volumeProvider) {
        if (volumeProvider == null) throw new IllegalArgumentException("volumeProvider may not be null!");
        this.volumeProvider = volumeProvider;
        notifyControllers(cb -> cb.onAudioInfoChanged(MediaController.playbackInfo(this)));
    }

    public void setActive(boolean active) { this.active = active; }
    public boolean isActive() { return active; }

    public void sendSessionEvent(String event, Bundle extras) {
        if (event == null || event.isEmpty()) throw new IllegalArgumentException("event cannot be null or empty");
        notifyControllers(cb -> cb.onSessionEvent(event, extras));
    }

    public void release() {
        released = true;
        active = false;
        notifyControllers(MediaController.Callback::onSessionDestroyed);
        synchronized (controllerCallbacks) { controllerCallbacks.clear(); }
    }

    public Token getSessionToken() { return token; }
    public MediaController getController() { return new MediaController(context, token); }

    public void setPlaybackState(PlaybackState state) {
        this.state = state;
        notifyControllers(cb -> cb.onPlaybackStateChanged(state));
    }

    public void setMetadata(MediaMetadata metadata) {
        this.metadata = metadata;
        notifyControllers(cb -> cb.onMetadataChanged(metadata));
    }

    public void setQueue(List<QueueItem> queue) {
        this.queue = queue != null ? new ArrayList<>(queue) : null;
        List<QueueItem> q = this.queue;
        notifyControllers(cb -> cb.onQueueChanged(q));
    }

    public void setQueueTitle(CharSequence title) {
        queueTitle = title;
        notifyControllers(cb -> cb.onQueueTitleChanged(title));
    }

    public void setRatingType(int type) { ratingType = type; }

    public void setExtras(Bundle extras) {
        this.extras = extras;
        notifyControllers(cb -> cb.onExtrasChanged(extras));
    }

    /** Every controller is the app's own. */
    public final MediaSessionManager.RemoteUserInfo getCurrentControllerInfo() {
        return new MediaSessionManager.RemoteUserInfo(packageName(), Process.myPid(), Process.myUid());
    }

    public static final class Token implements Parcelable {
        final MediaSession session;

        Token(MediaSession session) { this.session = session; }

        @Override public boolean equals(Object o) { return o instanceof Token t && t.session == session; }
        @Override public int hashCode() { return System.identityHashCode(session); }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
        public static final Parcelable.Creator<Token> CREATOR = new Parcelable.Creator<>() {
            @Override public Token createFromParcel(Parcel in) { return (Token) in.readValue(null); }
            @Override public Token[] newArray(int size) { return new Token[size]; }
        };
    }

    public abstract static class Callback {
        MediaSession session;

        public Callback() {}

        public void onCommand(String command, Bundle args, ResultReceiver cb) {}

        /** A media key: the transport callback that the key names. Play-pause plays or pauses by the session's state. */
        public boolean onMediaButtonEvent(Intent mediaButtonIntent) {
            if (session == null || !ACTION_MEDIA_BUTTON.equals(mediaButtonIntent.getAction())) return false;
            KeyEvent ke = mediaButtonIntent.getParcelableExtra(EXTRA_KEY_EVENT);
            if (ke == null || ke.getAction() != KeyEvent.ACTION_DOWN) return false;
            PlaybackState state = session.state;
            long actions = state != null ? state.getActions() : 0;
            switch (ke.getKeyCode()) {
                case 126 -> { if ((actions & PlaybackState.ACTION_PLAY) != 0) onPlay(); }
                case 127 -> { if ((actions & PlaybackState.ACTION_PAUSE) != 0) onPause(); }
                case 87 -> { if ((actions & PlaybackState.ACTION_SKIP_TO_NEXT) != 0) onSkipToNext(); }
                case 88 -> { if ((actions & PlaybackState.ACTION_SKIP_TO_PREVIOUS) != 0) onSkipToPrevious(); }
                case 86 -> { if ((actions & PlaybackState.ACTION_STOP) != 0) onStop(); }
                case 90 -> { if ((actions & PlaybackState.ACTION_FAST_FORWARD) != 0) onFastForward(); }
                case 89 -> { if ((actions & PlaybackState.ACTION_REWIND) != 0) onRewind(); }
                case 85, 79 -> {
                    boolean playing = state != null && state.getState() == PlaybackState.STATE_PLAYING;
                    boolean canPlay = (actions & (PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_PLAY)) != 0;
                    boolean canPause = (actions & (PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_PAUSE)) != 0;
                    if (playing && canPause) onPause();
                    else if (!playing && canPlay) onPlay();
                }
                default -> { return false; }
            }
            return true;
        }

        public void onPrepare() {}
        public void onPrepareFromMediaId(String mediaId, Bundle extras) {}
        public void onPrepareFromSearch(String query, Bundle extras) {}
        public void onPrepareFromUri(Uri uri, Bundle extras) {}
        public void onPlay() {}
        public void onPlayFromSearch(String query, Bundle extras) {}
        public void onPlayFromMediaId(String mediaId, Bundle extras) {}
        public void onPlayFromUri(Uri uri, Bundle extras) {}
        public void onSkipToQueueItem(long id) {}
        public void onPause() {}
        public void onSkipToNext() {}
        public void onSkipToPrevious() {}
        public void onFastForward() {}
        public void onRewind() {}
        public void onStop() {}
        public void onSeekTo(long pos) {}
        public void onSetRating(Rating rating) {}
        public void onSetPlaybackSpeed(float speed) {}
        public void onCustomAction(String action, Bundle extras) {}
    }

    public static final class QueueItem implements Parcelable {
        public static final int UNKNOWN_ID = -1;

        private final MediaDescription description;
        private final long id;

        public QueueItem(MediaDescription description, long id) {
            if (description == null) throw new IllegalArgumentException("Description cannot be null.");
            if (id == UNKNOWN_ID) throw new IllegalArgumentException("Id cannot be QueueItem.UNKNOWN_ID");
            this.description = description;
            this.id = id;
        }

        public MediaDescription getDescription() { return description; }
        public long getQueueId() { return id; }

        @Override public boolean equals(Object o) { return o instanceof QueueItem q && q.id == id && java.util.Objects.equals(q.description, description); }
        @Override public int hashCode() { return Long.hashCode(id); }
        @Override public String toString() { return "MediaSession.QueueItem {Description=" + description + ", Id=" + id + " }"; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) { description.writeToParcel(dest, flags); dest.writeLong(id); }
        public static final Parcelable.Creator<QueueItem> CREATOR = new Parcelable.Creator<>() {
            @Override public QueueItem createFromParcel(Parcel in) { return new QueueItem(MediaDescription.CREATOR.createFromParcel(in), in.readLong()); }
            @Override public QueueItem[] newArray(int size) { return new QueueItem[size]; }
        };
    }
}
