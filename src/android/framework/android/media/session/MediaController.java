// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2014 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.media.session;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.Rating;
import android.media.VolumeProvider;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.ResultReceiver;
import android.view.KeyEvent;
import java.util.List;

/** Controls a media session of the process, through its token. */
public final class MediaController {
    private final MediaSession session;
    private final TransportControls transportControls = new TransportControls();

    public MediaController(Context context, MediaSession.Token token) {
        if (context == null) throw new IllegalArgumentException("context shouldn't be null");
        if (token == null) throw new IllegalArgumentException("token shouldn't be null");
        session = token.session;
    }

    public TransportControls getTransportControls() { return transportControls; }

    private static boolean isMediaKey(int keyCode) {
        return switch (keyCode) {
            case 79, 85, 86, 87, 88, 89, 90, 126, 127, 130 -> true;
            default -> false;
        };
    }

    public boolean dispatchMediaButtonEvent(KeyEvent keyEvent) {
        if (keyEvent == null) throw new IllegalArgumentException("KeyEvent may not be null");
        if (!isMediaKey(keyEvent.getKeyCode())) return false;
        Intent intent = new Intent(MediaSession.ACTION_MEDIA_BUTTON).putExtra(MediaSession.EXTRA_KEY_EVENT, keyEvent);
        session.dispatch(cb -> cb.onMediaButtonEvent(intent));
        return true;
    }

    public PlaybackState getPlaybackState() { return session.state; }
    public MediaMetadata getMetadata() { return session.metadata; }
    public List<MediaSession.QueueItem> getQueue() { return session.queue; }
    public CharSequence getQueueTitle() { return session.queueTitle; }
    public Bundle getExtras() { return session.extras; }
    public int getRatingType() { return session.ratingType; }
    public long getFlags() { return session.flags; }
    public PlaybackInfo getPlaybackInfo() { return playbackInfo(session); }
    public PendingIntent getSessionActivity() { return session.sessionActivity; }
    public MediaSession.Token getSessionToken() { return session.token; }
    public String getPackageName() { return session.packageName(); }
    public String getTag() { return session.tag; }
    public Bundle getSessionInfo() { return new Bundle(); }

    static PlaybackInfo playbackInfo(MediaSession s) {
        VolumeProvider vp = s.volumeProvider;
        if (vp != null) {
            return new PlaybackInfo(PlaybackInfo.PLAYBACK_TYPE_REMOTE, vp.getVolumeControl(), vp.getMaxVolume(), vp.getCurrentVolume(),
                new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build());
        }
        AudioManager am = new AudioManager();
        int stream = s.localAttributes.getVolumeControlStream();
        return new PlaybackInfo(PlaybackInfo.PLAYBACK_TYPE_LOCAL, VolumeProvider.VOLUME_CONTROL_ABSOLUTE,
            am.getStreamMaxVolume(stream), am.getStreamVolume(stream), s.localAttributes);
    }

    public void setVolumeTo(int value, int flags) {
        VolumeProvider vp = session.volumeProvider;
        if (vp != null) {
            if (vp.getVolumeControl() == VolumeProvider.VOLUME_CONTROL_ABSOLUTE) vp.onSetVolumeTo(value);
        } else {
            new AudioManager().setStreamVolume(session.localAttributes.getVolumeControlStream(), value, flags);
        }
    }

    public void adjustVolume(int direction, int flags) {
        VolumeProvider vp = session.volumeProvider;
        if (vp != null) {
            if (vp.getVolumeControl() != VolumeProvider.VOLUME_CONTROL_FIXED) vp.onAdjustVolume(direction);
        } else {
            new AudioManager().adjustStreamVolume(session.localAttributes.getVolumeControlStream(), direction, flags);
        }
    }

    public void registerCallback(Callback callback) { registerCallback(callback, null); }

    public void registerCallback(Callback callback, Handler handler) {
        if (callback == null) throw new IllegalArgumentException("callback must not be null");
        Looper looper = Looper.myLooper();
        Handler h = handler != null ? handler : new Handler(looper != null ? looper : Looper.getMainLooper());
        synchronized (session.controllerCallbacks) {
            for (Object[] e : session.controllerCallbacks) if (e[0] == callback) return;
            session.controllerCallbacks.add(new Object[] { callback, h });
        }
    }

    public void unregisterCallback(Callback callback) {
        if (callback == null) throw new IllegalArgumentException("callback must not be null");
        synchronized (session.controllerCallbacks) { session.controllerCallbacks.removeIf(e -> e[0] == callback); }
    }

    public void sendCommand(String command, Bundle args, ResultReceiver cb) {
        if (command == null || command.isEmpty()) throw new IllegalArgumentException("command cannot be null or empty");
        session.dispatch(c -> c.onCommand(command, args, cb));
    }

    public abstract static class Callback {
        public Callback() {}
        public void onSessionDestroyed() {}
        public void onSessionEvent(String event, Bundle extras) {}
        public void onPlaybackStateChanged(PlaybackState state) {}
        public void onMetadataChanged(MediaMetadata metadata) {}
        public void onQueueChanged(List<MediaSession.QueueItem> queue) {}
        public void onQueueTitleChanged(CharSequence title) {}
        public void onExtrasChanged(Bundle extras) {}
        public void onAudioInfoChanged(PlaybackInfo info) {}
    }

    public final class TransportControls {
        private TransportControls() {}

        public void prepare() { session.dispatch(MediaSession.Callback::onPrepare); }
        public void prepareFromMediaId(String mediaId, Bundle extras) {
            if (mediaId == null || mediaId.isEmpty()) throw new IllegalArgumentException("You must specify a non-empty String for prepareFromMediaId.");
            session.dispatch(c -> c.onPrepareFromMediaId(mediaId, extras));
        }
        public void prepareFromSearch(String query, Bundle extras) { session.dispatch(c -> c.onPrepareFromSearch(query != null ? query : "", extras)); }
        public void prepareFromUri(Uri uri, Bundle extras) {
            if (uri == null || Uri.EMPTY.equals(uri)) throw new IllegalArgumentException("You must specify a non-empty Uri for prepareFromUri.");
            session.dispatch(c -> c.onPrepareFromUri(uri, extras));
        }
        public void play() { session.dispatch(MediaSession.Callback::onPlay); }
        public void playFromMediaId(String mediaId, Bundle extras) {
            if (mediaId == null || mediaId.isEmpty()) throw new IllegalArgumentException("You must specify a non-empty String for playFromMediaId.");
            session.dispatch(c -> c.onPlayFromMediaId(mediaId, extras));
        }
        public void playFromSearch(String query, Bundle extras) { session.dispatch(c -> c.onPlayFromSearch(query != null ? query : "", extras)); }
        public void playFromUri(Uri uri, Bundle extras) {
            if (uri == null || Uri.EMPTY.equals(uri)) throw new IllegalArgumentException("You must specify a non-empty Uri for playFromUri.");
            session.dispatch(c -> c.onPlayFromUri(uri, extras));
        }
        public void skipToQueueItem(long id) { session.dispatch(c -> c.onSkipToQueueItem(id)); }
        public void pause() { session.dispatch(MediaSession.Callback::onPause); }
        public void stop() { session.dispatch(MediaSession.Callback::onStop); }
        public void seekTo(long pos) { session.dispatch(c -> c.onSeekTo(pos)); }
        public void fastForward() { session.dispatch(MediaSession.Callback::onFastForward); }
        public void skipToNext() { session.dispatch(MediaSession.Callback::onSkipToNext); }
        public void rewind() { session.dispatch(MediaSession.Callback::onRewind); }
        public void skipToPrevious() { session.dispatch(MediaSession.Callback::onSkipToPrevious); }
        public void setRating(Rating rating) { session.dispatch(c -> c.onSetRating(rating)); }
        public void setPlaybackSpeed(float speed) {
            if (speed == 0f) throw new IllegalArgumentException("speed must not be zero");
            session.dispatch(c -> c.onSetPlaybackSpeed(speed));
        }
        public void sendCustomAction(PlaybackState.CustomAction customAction, Bundle args) {
            if (customAction == null) throw new IllegalArgumentException("CustomAction cannot be null.");
            sendCustomAction(customAction.getAction(), args);
        }
        public void sendCustomAction(String action, Bundle args) {
            if (action == null || action.isEmpty()) throw new IllegalArgumentException("CustomAction cannot be null.");
            session.dispatch(c -> c.onCustomAction(action, args));
        }
    }

    public static final class PlaybackInfo implements Parcelable {
        public static final int PLAYBACK_TYPE_LOCAL = 1, PLAYBACK_TYPE_REMOTE = 2;

        private final int type, control, max, current;
        private final AudioAttributes attributes;

        PlaybackInfo(int type, int control, int max, int current, AudioAttributes attributes) {
            this.type = type; this.control = control; this.max = max; this.current = current; this.attributes = attributes;
        }

        public int getPlaybackType() { return type; }
        public AudioAttributes getAudioAttributes() { return attributes; }
        public int getVolumeControl() { return control; }
        public int getMaxVolume() { return max; }
        public int getCurrentVolume() { return current; }
        public String getVolumeControlId() { return null; }

        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
        public static final Parcelable.Creator<PlaybackInfo> CREATOR = new Parcelable.Creator<>() {
            @Override public PlaybackInfo createFromParcel(Parcel in) { return (PlaybackInfo) in.readValue(null); }
            @Override public PlaybackInfo[] newArray(int size) { return new PlaybackInfo[size]; }
        };
    }
}
