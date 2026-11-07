// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.media;

import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceHolder;
import java.io.FileDescriptor;
import java.io.IOException;

/**
 * Plays audio and video. No decoder is here: each source prepares as a clip with no length and no picture, and
 * playback completes at once, so an app's flow goes on as after a short clip.
 */
public class MediaPlayer {
    public static final int MEDIA_ERROR_UNKNOWN = 1, MEDIA_ERROR_SERVER_DIED = 100, MEDIA_ERROR_IO = -1004,
        MEDIA_ERROR_MALFORMED = -1007, MEDIA_ERROR_UNSUPPORTED = -1010, MEDIA_ERROR_TIMED_OUT = -110;
    public static final int MEDIA_INFO_UNKNOWN = 1, MEDIA_INFO_VIDEO_RENDERING_START = 3, MEDIA_INFO_BUFFERING_START = 701,
        MEDIA_INFO_BUFFERING_END = 702;
    public static final int SEEK_PREVIOUS_SYNC = 0, SEEK_NEXT_SYNC = 1, SEEK_CLOSEST_SYNC = 2, SEEK_CLOSEST = 3;

    public interface OnPreparedListener { void onPrepared(MediaPlayer mp); }
    public interface OnCompletionListener { void onCompletion(MediaPlayer mp); }
    public interface OnBufferingUpdateListener { void onBufferingUpdate(MediaPlayer mp, int percent); }
    public interface OnSeekCompleteListener { void onSeekComplete(MediaPlayer mp); }
    public interface OnVideoSizeChangedListener { void onVideoSizeChanged(MediaPlayer mp, int width, int height); }
    public interface OnErrorListener { boolean onError(MediaPlayer mp, int what, int extra); }
    public interface OnInfoListener { boolean onInfo(MediaPlayer mp, int what, int extra); }

    private static final int IDLE = 0, INITIALIZED = 1, PREPARING = 2, PREPARED = 3, STARTED = 4, PAUSED = 5,
        COMPLETED = 6, STOPPED = 7, END = 8;

    private final Handler handler;
    private int state = IDLE, sessionId = AudioTrack.newSessionId();
    private boolean looping;
    private OnPreparedListener onPrepared;
    private OnCompletionListener onCompletion;
    private OnBufferingUpdateListener onBuffering;
    private OnSeekCompleteListener onSeek;
    private OnVideoSizeChangedListener onVideoSize;
    private OnErrorListener onError;
    private OnInfoListener onInfo;

    public MediaPlayer() {
        Looper looper = Looper.myLooper();
        handler = new Handler(looper != null ? looper : Looper.getMainLooper());
    }

    public static MediaPlayer create(Context context, Uri uri) {
        MediaPlayer mp = new MediaPlayer();
        try {
            mp.setDataSource(context, uri);
            mp.prepare();
            return mp;
        } catch (IOException e) {
            return null;
        }
    }

    public static MediaPlayer create(Context context, int resid) {
        MediaPlayer mp = new MediaPlayer();
        mp.state = INITIALIZED;
        mp.prepareNow();
        return mp;
    }

    private void source() {
        if (state != IDLE) throw new IllegalStateException("setDataSource called in state " + state);
        state = INITIALIZED;
    }

    public void setDataSource(Context context, Uri uri) throws IOException { source(); }
    public void setDataSource(String path) throws IOException { source(); }
    public void setDataSource(FileDescriptor fd) throws IOException { source(); }
    public void setDataSource(FileDescriptor fd, long offset, long length) throws IOException { source(); }
    public void setDataSource(android.content.res.AssetFileDescriptor afd) throws IOException { source(); }

    private void prepareNow() {
        state = PREPARED;
        if (onVideoSize != null) handler.post(() -> onVideoSize.onVideoSizeChanged(this, 0, 0));
    }

    public void prepare() throws IOException {
        if (state != INITIALIZED && state != STOPPED) throw new IllegalStateException("prepare called in state " + state);
        prepareNow();
    }

    public void prepareAsync() {
        if (state != INITIALIZED && state != STOPPED) throw new IllegalStateException("prepareAsync called in state " + state);
        state = PREPARING;
        handler.post(() -> {
            if (state != PREPARING) return;
            prepareNow();
            if (onBuffering != null) onBuffering.onBufferingUpdate(this, 100);
            if (onPrepared != null) onPrepared.onPrepared(this);
        });
    }

    public void start() {
        if (state != PREPARED && state != STARTED && state != PAUSED && state != COMPLETED) {
            error(MEDIA_ERROR_UNKNOWN, -38);
            return;
        }
        state = STARTED;
        if (!looping) handler.post(() -> {
            if (state != STARTED) return;
            state = COMPLETED;
            if (onCompletion != null) onCompletion.onCompletion(this);
        });
    }

    /** An error in a wrong state, as Android reports it: to onError, or to onCompletion when onError does not take it. */
    private void error(int what, int extra) {
        handler.post(() -> {
            boolean handled = onError != null && onError.onError(this, what, extra);
            if (!handled && onCompletion != null) onCompletion.onCompletion(this);
        });
    }

    public void pause() { if (state == STARTED) state = PAUSED; }
    public void stop() { if (state >= PREPARED && state <= STOPPED) state = STOPPED; }
    public void reset() { state = IDLE; }
    public void release() { state = END; }

    public void seekTo(int msec) { seekTo(msec, SEEK_PREVIOUS_SYNC); }
    public void seekTo(long msec, int mode) {
        if (state < PREPARED || state == STOPPED || state == END) throw new IllegalStateException("seekTo called in state " + state);
        if (onSeek != null) handler.post(() -> onSeek.onSeekComplete(this));
    }

    public boolean isPlaying() { return state == STARTED; }
    public int getCurrentPosition() { return 0; }
    public int getDuration() { return state >= PREPARED && state <= COMPLETED ? 0 : -1; }
    public int getVideoWidth() { return 0; }
    public int getVideoHeight() { return 0; }
    public int getAudioSessionId() { return sessionId; }
    public void setAudioSessionId(int sessionId) {
        if (state != IDLE) throw new IllegalStateException("setAudioSessionId called in state " + state);
        this.sessionId = sessionId;
    }

    public boolean isLooping() { return looping; }
    public void setLooping(boolean looping) { this.looping = looping; }
    public void setVolume(float leftVolume, float rightVolume) {}
    @Deprecated public void setAudioStreamType(int streamtype) {}
    public void setAudioAttributes(AudioAttributes attributes) {
        if (attributes == null) throw new IllegalArgumentException("Cannot set AudioAttributes to null");
    }
    public void setDisplay(SurfaceHolder sh) {}
    public void setSurface(android.view.Surface surface) {}
    public void setScreenOnWhilePlaying(boolean screenOn) {}
    public void setWakeMode(Context context, int mode) {}

    public void setOnPreparedListener(OnPreparedListener listener) { onPrepared = listener; }
    public void setOnCompletionListener(OnCompletionListener listener) { onCompletion = listener; }
    public void setOnBufferingUpdateListener(OnBufferingUpdateListener listener) { onBuffering = listener; }
    public void setOnSeekCompleteListener(OnSeekCompleteListener listener) { onSeek = listener; }
    public void setOnVideoSizeChangedListener(OnVideoSizeChangedListener listener) { onVideoSize = listener; }
    public void setOnErrorListener(OnErrorListener listener) { onError = listener; }
    public void setOnInfoListener(OnInfoListener listener) { onInfo = listener; }
}
