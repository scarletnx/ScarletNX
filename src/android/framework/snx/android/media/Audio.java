// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.media;

import android.content.Context;
import android.net.Uri;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.util.HashMap;
import java.util.Map;

/**
 * The runtime's audio (src/android/runtime/audio.c), which MediaPlayer and SoundPool play through: a source's bytes,
 * read here, are decoded by FFmpeg, and one SDL audio stream mixes the voices. A voice is an ID: a sound's (which ends
 * by itself) or a stream's (a player's, which stays paused at its end). A thread tells each watched voice's end.
 */
public final class Audio {
    public static final int PAUSE = 0, RESUME = 1, STOP = 2, VOLUME = 3, LOOPS = 4, RATE = 5, SEEK = 6;

    private Audio() {}

    /** a sound decoded whole, 0 when the source holds no audio that the runtime decodes */
    public static native long nativeLoad(byte[] data);
    /** the sound given up; the voices that play it keep it to their end */
    public static native void nativeUnload(long sound);
    /** a voice of the sound, 0 when none plays: loops more plays after the first, -1 for ever */
    public static native int nativePlay(long sound, float left, float right, int loops, float rate);
    /** a paused voice of a stream, decoded as it plays, and its length in milliseconds (-1 unknown) in duration[0] */
    public static native int nativeOpen(byte[] data, long[] duration);
    /** the voice paused, resumed, stopped, its volumes (x, y), loops (x), rate (x) or position in milliseconds (x) */
    public static native void nativeSet(int voice, int what, double x, double y);
    /** the voice's position in milliseconds, -1 when it is gone: completed[0] tells once that it reached its end */
    public static native long nativePosition(int voice, boolean[] completed);

    public static void set(int voice, int what, double x, double y) {
        if (voice != 0) nativeSet(voice, what, x, y);
    }
    public static void set(int voice, int what) { set(voice, what, 0, 0); }
    public static float volume(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

    // ---- the sources ----

    public static byte[] read(InputStream in) throws IOException {
        try (InputStream s = in) { return s.readAllBytes(); }
    }
    public static byte[] read(String path) throws IOException { return java.nio.file.Files.readAllBytes(java.nio.file.Path.of(path)); }
    public static byte[] read(Context context, int resId) throws IOException {
        return read(context.getResources().openRawResource(resId));
    }
    /** length bytes from offset (to the end for a negative or too large length), the descriptor left open */
    public static byte[] read(FileDescriptor fd, long offset, long length) throws IOException {
        FileChannel channel = new FileInputStream(fd).getChannel();
        long end = channel.size();
        long n = length < 0 || offset + length > end || offset + length < 0 ? end - offset : length;
        if (n < 0 || n > Integer.MAX_VALUE - 8) throw new IOException("bad source range");
        ByteBuffer b = ByteBuffer.allocate((int) n);
        while (b.hasRemaining() && channel.read(b, offset + b.position()) > 0) {}
        return b.array();
    }
    /** a file:, android.resource: or content: URI's bytes, or a path's */
    public static byte[] read(Context context, Uri uri) throws IOException {
        String scheme = uri.getScheme();
        if (scheme == null || "file".equals(scheme)) return read(uri.getPath());
        if ("android.resource".equals(scheme)) {
            java.util.List<String> parts = uri.getPathSegments();
            String pkg = uri.getAuthority();
            int id = 0;
            if (parts.size() == 1) {
                try {
                    id = Integer.parseInt(parts.get(0));
                } catch (NumberFormatException e) {
                    id = 0;
                }
            } else if (parts.size() == 2) {
                id = context.getResources().getIdentifier(parts.get(1), parts.get(0), pkg);
            }
            if (id == 0) throw new IOException("no resource for " + uri);
            return read(context, id);
        }
        return read(context.getContentResolver().openInputStream(uri));
    }

    // ---- the ends of watched voices ----

    private static final Map<Integer, Runnable> watched = new HashMap<>();
    private static Thread watcher;

    /** onEnd run (on the watcher's thread) each time the voice reaches its end, until the voice is gone or unwatched */
    public static synchronized void watch(int voice, Runnable onEnd) {
        if (voice == 0) return;
        watched.put(voice, onEnd);
        if (watcher == null) {
            watcher = new Thread(Audio::watchLoop, "snx-audio");
            watcher.setDaemon(true);
            watcher.start();
        }
    }
    public static synchronized void unwatch(int voice) { watched.remove(voice); }

    private static void watchLoop() {
        boolean[] completed = new boolean[1];
        while (true) {
            Map<Integer, Runnable> now;
            synchronized (Audio.class) { now = new HashMap<>(watched); }
            for (Map.Entry<Integer, Runnable> e : now.entrySet()) {
                completed[0] = false;
                long position = nativePosition(e.getKey(), completed);
                if (completed[0]) e.getValue().run();
                if (position < 0) unwatch(e.getKey());
            }
            try {
                Thread.sleep(20);
            } catch (InterruptedException e) {
                return;
            }
        }
    }
}
