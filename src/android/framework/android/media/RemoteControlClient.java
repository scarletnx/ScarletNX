// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.app.PendingIntent;
import android.graphics.Bitmap;
import android.os.Looper;

/** The old remote control API. No lock screen or remote shows it here, so its state stays in the client. */
@Deprecated
public class RemoteControlClient {
    public static final int PLAYSTATE_STOPPED = 1, PLAYSTATE_PAUSED = 2, PLAYSTATE_PLAYING = 3, PLAYSTATE_FAST_FORWARDING = 4,
        PLAYSTATE_REWINDING = 5, PLAYSTATE_SKIPPING_FORWARDS = 6, PLAYSTATE_SKIPPING_BACKWARDS = 7, PLAYSTATE_BUFFERING = 8,
        PLAYSTATE_ERROR = 9;
    public static final int FLAG_KEY_MEDIA_PREVIOUS = 1, FLAG_KEY_MEDIA_REWIND = 2, FLAG_KEY_MEDIA_PLAY = 4,
        FLAG_KEY_MEDIA_PLAY_PAUSE = 8, FLAG_KEY_MEDIA_PAUSE = 16, FLAG_KEY_MEDIA_STOP = 32, FLAG_KEY_MEDIA_FAST_FORWARD = 64,
        FLAG_KEY_MEDIA_NEXT = 128, FLAG_KEY_MEDIA_POSITION_UPDATE = 256, FLAG_KEY_MEDIA_RATING = 512;

    public interface OnMetadataUpdateListener { void onMetadataUpdate(int key, Object newValue); }
    public interface OnPlaybackPositionUpdateListener { void onPlaybackPositionUpdate(long newPositionMs); }
    public interface OnGetPlaybackPositionListener { long onGetPlaybackPosition(); }

    private final PendingIntent mediaButtonIntent;
    private int playbackState = PLAYSTATE_STOPPED, transportControlFlags;
    private long playbackPosition;
    private float playbackSpeed = 1f;
    private OnMetadataUpdateListener metadataListener;
    private OnPlaybackPositionUpdateListener positionListener;

    public RemoteControlClient(PendingIntent mediaButtonIntent) { this.mediaButtonIntent = mediaButtonIntent; }
    public RemoteControlClient(PendingIntent mediaButtonIntent, Looper looper) { this(mediaButtonIntent); }

    public PendingIntent getMediaButtonIntent() { return mediaButtonIntent; }

    public MetadataEditor editMetadata(boolean startEmpty) { return new MetadataEditor(); }

    public void setPlaybackState(int state) { setPlaybackState(state, -1, 1f); }
    public void setPlaybackState(int state, long timeInMs, float playbackSpeed) {
        playbackState = state;
        playbackPosition = timeInMs;
        this.playbackSpeed = playbackSpeed;
    }
    public void setTransportControlFlags(int transportControlFlags) { this.transportControlFlags = transportControlFlags; }
    public void setMetadataUpdateListener(OnMetadataUpdateListener l) { metadataListener = l; }
    public void setPlaybackPositionUpdateListener(OnPlaybackPositionUpdateListener l) { positionListener = l; }
    public void setOnGetPlaybackPositionListener(OnGetPlaybackPositionListener l) {}

    @Deprecated
    public class MetadataEditor extends MediaMetadataEditor {
        public static final int BITMAP_KEY_ARTWORK = 100;

        private MetadataEditor() {}

        @Override public synchronized MetadataEditor putString(int key, String value) { super.putString(key, value); return this; }
        @Override public synchronized MetadataEditor putLong(int key, long value) { super.putLong(key, value); return this; }
        @Override public synchronized MetadataEditor putBitmap(int key, Bitmap bitmap) { super.putBitmap(key, bitmap); return this; }
        @Override public synchronized MetadataEditor putObject(int key, Object object) { super.putObject(key, object); return this; }
        @Override public synchronized void clear() { super.clear(); }
        @Override public synchronized void apply() { mApplied = true; }
    }
}
