// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.os.Handler;
import android.os.Looper;
import java.io.FileDescriptor;

/**
 * Short sounds, loaded once and played many times. No decoder is here, as in MediaPlayer: a sound loads at once (the
 * listener hears of it, with status 0, on the looper of the thread that made the pool, as Android's does) and each
 * play gives a stream that ends at once, so an app's flow goes on as with sounds that play.
 */
public class SoundPool {
    public interface OnLoadCompleteListener {
        void onLoadComplete(SoundPool soundPool, int sampleId, int status);
    }

    public static class Builder {
        private int maxStreams = 1;
        private AudioAttributes attributes;

        public Builder() {}
        public Builder setMaxStreams(int maxStreams) {
            if (maxStreams <= 0) throw new IllegalArgumentException("Strictly positive value required for the maximum number of streams");
            this.maxStreams = maxStreams;
            return this;
        }
        public Builder setAudioAttributes(AudioAttributes attributes) {
            if (attributes == null) throw new IllegalArgumentException("Invalid null AudioAttributes");
            this.attributes = attributes;
            return this;
        }
        public Builder setAudioSessionId(int sessionId) { return this; }
        public Builder setContext(Context context) { return this; }
        public SoundPool build() {
            return new SoundPool(maxStreams, attributes != null ? attributes : new AudioAttributes.Builder().build());
        }
    }

    private final Handler handler;
    private final int maxStreams;
    private OnLoadCompleteListener listener;
    private int nextSample = 1, nextStream = 1;
    private boolean released;

    /** @deprecated use {@link Builder} */
    @Deprecated
    public SoundPool(int maxStreams, int streamType, int srcQuality) {
        this(maxStreams, new AudioAttributes.Builder().build());
    }

    private SoundPool(int maxStreams, AudioAttributes attributes) {
        this.maxStreams = maxStreams;
        Looper looper = Looper.myLooper();
        handler = new Handler(looper != null ? looper : Looper.getMainLooper());
    }

    public void setOnLoadCompleteListener(OnLoadCompleteListener listener) { this.listener = listener; }

    private synchronized int loaded() {
        if (released) return 0;
        int id = nextSample++;
        handler.post(() -> {
            OnLoadCompleteListener l = listener;
            if (l != null && !released) l.onLoadComplete(this, id, 0);
        });
        return id;
    }

    public int load(String path, int priority) { return loaded(); }
    public int load(Context context, int resId, int priority) { return loaded(); }
    public int load(AssetFileDescriptor afd, int priority) { return loaded(); }
    public int load(FileDescriptor fd, long offset, long length, int priority) { return loaded(); }
    public boolean unload(int soundId) { return soundId > 0 && soundId < nextSample; }

    /** A stream's ID, 0 when the sound is not one of the pool's */
    public synchronized int play(int soundId, float leftVolume, float rightVolume, int priority, int loop, float rate) {
        if (released || soundId <= 0 || soundId >= nextSample) return 0;
        return nextStream++;
    }
    public void pause(int streamId) {}
    public void resume(int streamId) {}
    public void autoPause() {}
    public void autoResume() {}
    public void stop(int streamId) {}
    public void setVolume(int streamId, float leftVolume, float rightVolume) {}
    public void setPriority(int streamId, int priority) {}
    public void setLoop(int streamId, int loop) {}
    public void setRate(int streamId, float rate) {}
    public synchronized void release() { released = true; }
}
