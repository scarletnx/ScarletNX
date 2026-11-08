// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.os.Handler;
import android.os.Looper;
import java.io.FileDescriptor;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Short sounds, loaded once and played many times, through the runtime's audio (snx.android.media.Audio). A load
 * reads the source and decodes it on the pool's thread, then tells the listener (status 0, or 1 when the source holds
 * no audio that decodes) on the looper of the thread that made the pool, as Android does. Each play is a stream: at
 * most maxStreams play at once, the oldest stopped for a new one.
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

    /** a source's bytes, read on the pool's thread */
    private interface Source {
        byte[] read() throws IOException;
    }

    private final Handler handler;
    private final int maxStreams;
    private final ExecutorService loader = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "SoundPool");
        t.setDaemon(true);
        return t;
    });
    private final HashMap<Integer, Long> sounds = new HashMap<>(); /* sample ID: the runtime's sound */
    private final ArrayList<Integer> streams = new ArrayList<>();   /* the streams played, oldest first */
    private final ArrayList<Integer> autoPaused = new ArrayList<>();
    private OnLoadCompleteListener listener;
    private int nextSample = 1;
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

    private synchronized int load(Source source) {
        if (released) return 0;
        int id = nextSample++;
        loader.execute(() -> {
            long sound = 0;
            try {
                sound = snx.android.media.Audio.nativeLoad(source.read());
            } catch (IOException | RuntimeException e) {
                android.util.Log.w("SoundPool", "cannot load sound " + id + ": " + e);
            }
            int status = sound != 0 ? 0 : 1;
            synchronized (this) {
                if (released || sound == 0) {
                    if (sound != 0) snx.android.media.Audio.nativeUnload(sound);
                } else {
                    sounds.put(id, sound);
                }
            }
            handler.post(() -> {
                OnLoadCompleteListener l = listener;
                if (l != null && !released) l.onLoadComplete(this, id, status);
            });
        });
        return id;
    }

    public int load(String path, int priority) { return load(() -> snx.android.media.Audio.read(path)); }
    public int load(Context context, int resId, int priority) { return load(() -> snx.android.media.Audio.read(context, resId)); }
    public int load(AssetFileDescriptor afd, int priority) {
        if (afd == null) return 0;
        FileDescriptor fd = afd.getFileDescriptor();
        long offset = afd.getStartOffset(), length = afd.getLength();
        byte[] data;
        try {
            data = snx.android.media.Audio.read(fd, offset, length); /* now: the app closes the descriptor after this */
        } catch (IOException e) {
            data = new byte[0];
        }
        byte[] bytes = data;
        return load(() -> bytes);
    }
    public int load(FileDescriptor fd, long offset, long length, int priority) {
        byte[] data;
        try {
            data = snx.android.media.Audio.read(fd, offset, length);
        } catch (IOException e) {
            data = new byte[0];
        }
        byte[] bytes = data;
        return load(() -> bytes);
    }
    public synchronized boolean unload(int soundId) {
        Long sound = sounds.remove(soundId);
        if (sound == null) return false;
        snx.android.media.Audio.nativeUnload(sound);
        return true;
    }

    /** A stream's ID, 0 when the sound is not loaded */
    public synchronized int play(int soundId, float leftVolume, float rightVolume, int priority, int loop, float rate) {
        Long sound = released ? null : sounds.get(soundId);
        if (sound == null) return 0;
        streams.removeIf(s -> snx.android.media.Audio.nativePosition(s, null) < 0);
        while (streams.size() >= maxStreams) snx.android.media.Audio.set(streams.remove(0), snx.android.media.Audio.STOP);
        float r = rate < 0.5f ? 0.5f : rate > 2f ? 2f : rate;
        int stream = snx.android.media.Audio.nativePlay(sound, snx.android.media.Audio.volume(leftVolume),
                                                       snx.android.media.Audio.volume(rightVolume), loop, r);
        if (stream != 0) streams.add(stream);
        return stream;
    }
    public void pause(int streamId) { snx.android.media.Audio.set(streamId, snx.android.media.Audio.PAUSE); }
    public void resume(int streamId) { snx.android.media.Audio.set(streamId, snx.android.media.Audio.RESUME); }
    public synchronized void autoPause() {
        for (int s : streams)
            if (snx.android.media.Audio.nativePosition(s, null) >= 0) {
                snx.android.media.Audio.set(s, snx.android.media.Audio.PAUSE);
                autoPaused.add(s);
            }
    }
    public synchronized void autoResume() {
        for (int s : autoPaused) snx.android.media.Audio.set(s, snx.android.media.Audio.RESUME);
        autoPaused.clear();
    }
    public synchronized void stop(int streamId) {
        snx.android.media.Audio.set(streamId, snx.android.media.Audio.STOP);
        streams.remove(Integer.valueOf(streamId));
    }
    public void setVolume(int streamId, float leftVolume, float rightVolume) {
        snx.android.media.Audio.set(streamId, snx.android.media.Audio.VOLUME, snx.android.media.Audio.volume(leftVolume),
                                    snx.android.media.Audio.volume(rightVolume));
    }
    public void setPriority(int streamId, int priority) {}
    public void setLoop(int streamId, int loop) { snx.android.media.Audio.set(streamId, snx.android.media.Audio.LOOPS, loop, 0); }
    public void setRate(int streamId, float rate) {
        snx.android.media.Audio.set(streamId, snx.android.media.Audio.RATE, rate < 0.5f ? 0.5f : rate > 2f ? 2f : rate, 0);
    }
    public synchronized void release() {
        released = true;
        for (int s : streams) snx.android.media.Audio.set(s, snx.android.media.Audio.STOP);
        streams.clear();
        for (long sound : sounds.values()) snx.android.media.Audio.nativeUnload(sound);
        sounds.clear();
        loader.shutdown();
    }
}
