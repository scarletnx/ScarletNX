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
import java.io.FileInputStream;
import java.io.IOException;
import snx.android.media.Audio;

/**
 * Plays audio, through the runtime's audio (snx.android.media.Audio): prepare reads the source and opens a stream of
 * it, which FFmpeg decodes as it plays. A source with no audio that decodes (a video's picture alone, a format the
 * runtime lacks) prepares as a clip with no length, and its playback completes at once, so an app's flow goes on as
 * after a short clip. No picture is drawn.
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

    /** a source's bytes, read when the player prepares */
    private interface Source {
        byte[] read() throws IOException;
    }

    private final Handler handler;
    private int state = IDLE, sessionId = AudioTrack.newSessionId();
    private boolean looping;
    private float left = 1, right = 1;
    private Source source;
    private int voice;     /* the runtime's stream, 0 for a source with no audio */
    private long duration; /* in milliseconds, -1 unknown */
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
        mp.source(() -> Audio.read(context, resid));
        try {
            mp.prepare();
            return mp;
        } catch (IOException e) {
            return null;
        }
    }

    private void source(Source s) {
        if (state != IDLE) throw new IllegalStateException("setDataSource called in state " + state);
        source = s;
        state = INITIALIZED;
    }

    public void setDataSource(Context context, Uri uri) throws IOException {
        String scheme = uri.getScheme();
        if ("file".equals(scheme)) setDataSource(uri.getPath());
        else if (scheme == null || scheme.equals("android.resource") || scheme.equals("content"))
            source(() -> Audio.read(context, uri));
        else source(() -> new byte[0]); /* a stream of the network (http, rtsp): no audio, a clip of length 0 */
    }

    /**
     * A path, or a file: URI, is opened as Android opens it before its decoder sees it, so a missing file is
     * FileNotFoundException; another scheme (http, rtsp) is taken as it is, with no audio. A drive's path ("C:\..."
     * or "C:/...") is a path, not a URI of scheme "C": the app's files have such paths on a Windows host
     */
    public void setDataSource(String path) throws IOException {
        String scheme = Uri.parse(path).getScheme();
        boolean drive = path.length() > 2 && (path.charAt(0) | 0x20) >= 'a' && (path.charAt(0) | 0x20) <= 'z'
            && path.charAt(1) == ':' && (path.charAt(2) == '\\' || path.charAt(2) == '/');
        if (scheme == null || drive || scheme.equals("file")) {
            String file = scheme == null || drive ? path : Uri.parse(path).getPath();
            try (FileInputStream in = new FileInputStream(file)) {
                source(() -> Audio.read(file));
            }
        } else {
            source(() -> new byte[0]);
        }
    }
    public void setDataSource(FileDescriptor fd) throws IOException { setDataSource(fd, 0, -1); }
    public void setDataSource(FileDescriptor fd, long offset, long length) throws IOException {
        byte[] data = Audio.read(fd, offset, length); /* now: the app may close the descriptor after this */
        source(() -> data);
    }
    public void setDataSource(android.content.res.AssetFileDescriptor afd) throws IOException {
        setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
    }

    /** the stream opened from the source: false when the source cannot be read */
    private boolean open() {
        close();
        byte[] data;
        try {
            data = source != null ? source.read() : new byte[0];
        } catch (IOException e) {
            android.util.Log.w("MediaPlayer", "cannot read the source: " + e);
            return false;
        }
        long[] length = {-1};
        voice = data.length > 0 ? Audio.nativeOpen(data, length) : 0;
        duration = voice != 0 ? length[0] : 0;
        if (voice != 0) {
            Audio.set(voice, Audio.VOLUME, left, right);
            Audio.set(voice, Audio.LOOPS, looping ? -1 : 0, 0);
            int v = voice;
            Audio.watch(v, () -> handler.post(() -> completed(v)));
        }
        state = PREPARED;
        if (onVideoSize != null) handler.post(() -> onVideoSize.onVideoSizeChanged(this, 0, 0));
        return true;
    }
    private void close() {
        if (voice == 0) return;
        Audio.unwatch(voice);
        Audio.set(voice, Audio.STOP);
        voice = 0;
    }

    public void prepare() throws IOException {
        if (state != INITIALIZED && state != STOPPED) throw new IllegalStateException("prepare called in state " + state);
        if (!open()) throw new IOException("Prepare failed.: status=0x1");
    }

    public void prepareAsync() {
        if (state != INITIALIZED && state != STOPPED) throw new IllegalStateException("prepareAsync called in state " + state);
        state = PREPARING;
        handler.post(() -> {
            if (state != PREPARING) return;
            if (!open()) {
                state = IDLE;
                error(MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_IO);
                return;
            }
            if (onBuffering != null) onBuffering.onBufferingUpdate(this, 100);
            if (onPrepared != null) onPrepared.onPrepared(this);
        });
    }

    /** the stream at its end, on the player's looper */
    private void completed(int v) {
        if (v != voice || state != STARTED || looping) return;
        state = COMPLETED;
        if (onCompletion != null) onCompletion.onCompletion(this);
    }

    public void start() {
        if (state != PREPARED && state != STARTED && state != PAUSED && state != COMPLETED) {
            error(MEDIA_ERROR_UNKNOWN, -38);
            return;
        }
        if (state == COMPLETED) Audio.set(voice, Audio.SEEK, 0, 0); /* from the start again, as Android plays it */
        state = STARTED;
        if (voice != 0) {
            Audio.set(voice, Audio.RESUME);
        } else if (!looping) {
            int v = voice;
            handler.post(() -> completed(v));
        }
    }

    /** An error in a wrong state, as Android reports it: to onError, or to onCompletion when onError does not take it. */
    private void error(int what, int extra) {
        handler.post(() -> {
            boolean handled = onError != null && onError.onError(this, what, extra);
            if (!handled && onCompletion != null) onCompletion.onCompletion(this);
        });
    }

    public void pause() {
        if (state != STARTED) return;
        state = PAUSED;
        Audio.set(voice, Audio.PAUSE);
    }
    public void stop() {
        if (state < PREPARED || state > STOPPED) return;
        state = STOPPED;
        Audio.set(voice, Audio.PAUSE);
        Audio.set(voice, Audio.SEEK, 0, 0);
    }
    public void reset() {
        close();
        source = null;
        state = IDLE;
    }
    public void release() {
        close();
        source = null;
        state = END;
    }

    public void seekTo(int msec) { seekTo(msec, SEEK_PREVIOUS_SYNC); }
    public void seekTo(long msec, int mode) {
        if (state < PREPARED || state == STOPPED || state == END) throw new IllegalStateException("seekTo called in state " + state);
        Audio.set(voice, Audio.SEEK, Math.max(0, msec), 0);
        if (state == COMPLETED) state = PAUSED;
        if (onSeek != null) handler.post(() -> onSeek.onSeekComplete(this));
    }

    public boolean isPlaying() { return state == STARTED; }
    public int getCurrentPosition() {
        if (voice == 0 || state < PREPARED || state == END) return 0;
        long ms = Audio.nativePosition(voice, null);
        return ms < 0 ? 0 : duration > 0 && ms > duration ? (int) duration : (int) ms;
    }
    public int getDuration() { return state >= PREPARED && state <= COMPLETED ? (int) duration : -1; }
    public int getVideoWidth() { return 0; }
    public int getVideoHeight() { return 0; }
    public int getAudioSessionId() { return sessionId; }
    public void setAudioSessionId(int sessionId) {
        if (state != IDLE) throw new IllegalStateException("setAudioSessionId called in state " + state);
        this.sessionId = sessionId;
    }

    public boolean isLooping() { return looping; }
    public void setLooping(boolean looping) {
        this.looping = looping;
        Audio.set(voice, Audio.LOOPS, looping ? -1 : 0, 0);
    }
    public void setVolume(float leftVolume, float rightVolume) {
        left = Audio.volume(leftVolume);
        right = Audio.volume(rightVolume);
        Audio.set(voice, Audio.VOLUME, left, right);
    }
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
