// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.ui;

import android.os.Looper;
import android.os.MessageQueue;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewRootImpl;

/**
 * The host window: an SDL window with the runtime's Vulkan renderer (src/android/hwui/render.c), the Abstract of
 * SurfaceFlinger and the input dispatcher in docs/platforms/android-apps/interfaces.md. Without a display the renderer draws into an
 * image of its own (--shot saves one). --headless draws nothing at all.
 *
 * The main looper waits in short steps and reads the window's events between them, since SDL's event queue belongs
 * to the thread that made the window, and that is the main thread.
 */
public final class Window {
    private static boolean open, failed;
    private static final boolean headless = Boolean.getBoolean("snx.android.headless");
    private static final float[] events = new float[8 * 64];
    private static long downTime;
    private static final int POLL_MS = 4;
    /** Scripted taps for runs without a user (--taps="FRAME:X,Y ..."): a finger down at the frame, up 3 later */
    private static final long[] tapFrames;
    private static final float[][] tapPoints;
    private static long frames;
    /** Scripted text (--keys="FRAME:TEXT|FRAME:TEXT"): its characters as key presses, from the frame on */
    private static final long[] keyFrames;
    /** The frames between scripted keys (--key-frames, 4 when not given: about 66 ms, a fast typist's) */
    private static final int keyPace = Math.max(1, Integer.parseInt(System.getProperty("snx.android.key_frames", "4")));
    private static final String[] keyTexts;
    static {
        String keys = System.getProperty("snx.android.keys");
        String[] parts = keys != null && !keys.isEmpty() ? keys.split("\\|") : new String[0];
        keyFrames = new long[parts.length];
        keyTexts = new String[parts.length];
        for (int i = 0; i < parts.length; ++i) {
            int colon = parts[i].indexOf(':');
            keyFrames[i] = Long.parseLong(parts[i].substring(0, colon));
            keyTexts[i] = parts[i].substring(colon + 1).replace("\\n", "\n");
        }
    }
    static {
        String script = System.getProperty("snx.android.taps");
        String[] items = script != null && !script.isBlank() ? script.trim().split("\\s+") : new String[0];
        tapFrames = new long[items.length];
        tapPoints = new float[items.length][];
        for (int i = 0; i < items.length; ++i) {
            String[] fx = items[i].split(":"), xy = fx[1].split(",");
            tapFrames[i] = Long.parseLong(fx[0]);
            tapPoints[i] = new float[] {Float.parseFloat(xy[0]), Float.parseFloat(xy[1])};
        }
    }

    /*
     * The scripts' frames are the app's own presents once it has them (a GLES or Vulkan surface's), as --shot-frame's
     * are: a game that draws on its own surface makes few frames of views. A thread follows their count.
     */
    private static volatile boolean appFrames;
    private static native long nativeAppPresents();
    static {
        if (tapFrames.length > 0 || keyFrames.length > 0) {
            Thread follower = new Thread(() -> {
                long seen = 0;
                try {
                    while (true) {
                        long n;
                        try {
                            n = nativeAppPresents();
                        } catch (UnsatisfiedLinkError e) {
                            n = 0; /* the class's natives come once the runtime registers them */
                        }
                        while (seen < n) {
                            appFrames = true;
                            script(++seen);
                            scriptKeys(seen);
                        }
                        Thread.sleep(2);
                    }
                } catch (InterruptedException e) {
                    // no presents to follow
                }
            }, "snx-script");
            follower.setDaemon(true);
            follower.start();
        }
    }

    /*
     * A script's frames are presents, and a still window presents nothing: while a tap, a key or a shot is still to
     * come and the app has no presents of its own, the last frame is presented again at the display's rate, as a
     * display keeps showing it.
     */
    private static volatile long lastPresent;
    private static final java.util.concurrent.atomic.AtomicBoolean refreshing = new java.util.concurrent.atomic.AtomicBoolean();
    static {
        long end = 0;
        for (long f : tapFrames) end = Math.max(end, f + 3);
        for (int i = 0; i < keyFrames.length; ++i) end = Math.max(end, keyFrames[i] + 2L * (keyTexts[i].length() + 1) * keyPace);
        String shots = System.getProperty("snx.android.shot_frames", "");
        for (String f : shots.split(",")) {
            try {
                end = Math.max(end, Long.parseLong(f.trim()));
            } catch (NumberFormatException e) {
                // not a frame
            }
        }
        long last = end;
        if (last > 0 && !headless) {
            Thread refresher = new Thread(() -> {
                try {
                    while (frames < last) {
                        Thread.sleep(16);
                        if (appFrames || !open || SystemClock.uptimeMillis() - lastPresent < 16 || !refreshing.compareAndSet(false, true))
                            continue;
                        new android.os.Handler(Looper.getMainLooper()).post(() -> {
                            refreshing.set(false);
                            if (SystemClock.uptimeMillis() - lastPresent >= 16) Host.refresh();
                        });
                    }
                } catch (InterruptedException e) {
                    // no more refreshes
                }
            }, "snx-refresh");
            refresher.setDaemon(true);
            refresher.start();
        }
    }

    private Window() {}

    static synchronized void present(Frame frame) {
        if (headless || failed) return;
        try {
            if (!open) {
                open = nativeOpen(frame.width, frame.height, snx.android.Main.processName());
                if (!open) {
                    failed = true;
                    android.util.Log.w("snx", "no Vulkan renderer: running headless");
                    return;
                }
                if (nativeHasWindow()) Looper.getMainLooper().getQueue().setWaiter(new Pump());
            }
            java.util.List<android.graphics.Bitmap> bitmaps = frame.bitmaps();
            int n = bitmaps.size();
            int[] info = new int[3 * n];
            for (int i = 0; i < n; ++i) {
                android.graphics.Bitmap b = bitmaps.get(i);
                info[3 * i] = b.id();
                info[3 * i + 1] = b.getWidth();
                info[3 * i + 2] = b.getHeight();
            }
            nativePresent(frame.width, frame.height, frame.bytes(), frame.size(), info,
                frame.uploads().toArray(new int[0][]), frame.drawings().toArray(new byte[0][]),
                android.graphics.Bitmap.takeFreed());
            lastPresent = SystemClock.uptimeMillis();
            if (!appFrames) {
                script(++frames);
                scriptKeys(frames);
            }
        } catch (UnsatisfiedLinkError e) {
            failed = true;
            android.util.Log.w("snx", "no renderer in this runtime: running headless");
        }
    }

    /**
     * The display turned to width by height (snx.android.Device.rotate): the host window takes the size (an SDL
     * window's size event follows, with the same size), and the display, its configuration and the windows' roots
     * take it now, so an app that waits for the new orientation sees it at once.
     */
    public static void rotate(int width, int height) {
        synchronized (Window.class) {
            if (open) nativeResize(width, height);
        }
        snx.android.Device.resize(width, height);
        Host.resize(width, height);
    }

    /** The main queue's wait: in steps, with the window's events read between them. */
    private static final class Pump implements MessageQueue.Waiter {
        @Override public void await(MessageQueue queue, long timeout) throws InterruptedException {
            long step = timeout < 0 || timeout > POLL_MS ? POLL_MS : timeout;
            if (step > 0) queue.wait(step);
            poll();
        }
        @Override public void wake(MessageQueue queue) { queue.notifyAll(); }
    }

    /** The scripted taps of a frame, posted to the main thread as the window's events are */
    private static void script(long frame) {
        for (int i = 0; i < tapFrames.length; ++i) {
            int action = frame == tapFrames[i] ? MotionEvent.ACTION_DOWN : frame == tapFrames[i] + 3 ? MotionEvent.ACTION_UP : -1;
            if (action < 0) continue;
            float x = tapPoints[i][0], y = tapPoints[i][1];
            new android.os.Handler(Looper.getMainLooper()).post(() -> {
                ViewRootImpl root = Host.top();
                if (root == null) return;
                long now = SystemClock.uptimeMillis();
                if (action == MotionEvent.ACTION_DOWN) downTime = now;
                MotionEvent e = MotionEvent.obtain(downTime, now, action, x, y, 0);
                e.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN);
                root.dispatchInputEvent(e);
            });
        }
    }

    /** A character as the key and modifiers that type it on a US keyboard: {code, meta}, or null */
    static int[] keyOf(char c) {
        if (c == '\n') return new int[] {KeyEvent.KEYCODE_ENTER, 0};
        for (int meta : new int[] {0, KeyEvent.META_SHIFT_ON})
            for (int code = 1; code < 300; ++code)
                if (new KeyEvent(0, 0, KeyEvent.ACTION_DOWN, code, 0, meta).getUnicodeChar() == c) return new int[] {code, meta};
        return null;
    }

    /**
     * The scripted text due at a frame: from its frame on, one character every keyPace frames, a press and a
     * release. A text field that the app controls takes each key through a recomposition, and a new line through
     * more, as a person's typing gives it time to
     */
    private static void scriptKeys(long frame) {
        for (int i = 0; i < keyFrames.length; ++i) {
            long k = frame - keyFrames[i];
            if (k < 0 || k % keyPace != 0 || k / keyPace >= keyTexts[i].length()) continue;
            char c = keyTexts[i].charAt((int) (k / keyPace));
            new android.os.Handler(Looper.getMainLooper()).post(() -> {
                int[] key = keyOf(c);
                ViewRootImpl root = Host.top();
                if (key == null || root == null) return;
                long now = SystemClock.uptimeMillis();
                root.dispatchInputEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, key[0], 0, key[1]));
                root.dispatchInputEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, key[0], 0, key[1]));
            });
        }
    }

    /** The window's events, each dispatched to the top window's root on the main thread. */
    static void poll() {
        int n = nativePoll(events);
        for (int i = 0; i < n; ++i) {
            int o = 8 * i, kind = (int) events[o], action = (int) events[o + 1];
            ViewRootImpl root = Host.top();
            long now = SystemClock.uptimeMillis();
            switch (kind) {
                case 1 -> {
                    if (root == null) break;
                    int a = action == 0 ? MotionEvent.ACTION_DOWN : action == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_MOVE;
                    if (a == MotionEvent.ACTION_DOWN) downTime = now;
                    MotionEvent e = MotionEvent.obtain(downTime, now, a, events[o + 2], events[o + 3], 0);
                    e.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN);
                    root.dispatchInputEvent(e);
                }
                case 2 -> {
                    if (root == null || action > 1) break;
                    int code = keyCode((int) events[o + 4]);
                    if (code == KeyEvent.KEYCODE_UNKNOWN) break;
                    int meta = meta((int) events[o + 5]);
                    root.dispatchInputEvent(new KeyEvent(now, now, action == 0 ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, code, 0, meta));
                }
                case 3 -> {
                    int w = (int) events[o + 2], h = (int) events[o + 3];
                    snx.android.Device.resize(w, h);
                    Host.resize(w, h);
                }
                case 4 -> snx.android.app.Activities.finishAll();
                default -> {}
            }
        }
    }

    /** SDL's key code as Android's, for a US keyboard */
    static int keyCode(int sdl) {
        if (sdl >= 'a' && sdl <= 'z') return KeyEvent.KEYCODE_A + (sdl - 'a');
        if (sdl >= '0' && sdl <= '9') return KeyEvent.KEYCODE_0 + (sdl - '0');
        return switch (sdl) {
            case '\r' -> KeyEvent.KEYCODE_ENTER;
            case '\b' -> KeyEvent.KEYCODE_DEL;
            case 127 -> KeyEvent.KEYCODE_FORWARD_DEL;
            case '\t' -> KeyEvent.KEYCODE_TAB;
            case 27 -> KeyEvent.KEYCODE_ESCAPE;
            case ' ' -> KeyEvent.KEYCODE_SPACE;
            case '-' -> KeyEvent.KEYCODE_MINUS;
            case '=' -> KeyEvent.KEYCODE_EQUALS;
            case '[' -> KeyEvent.KEYCODE_LEFT_BRACKET;
            case ']' -> KeyEvent.KEYCODE_RIGHT_BRACKET;
            case '\\' -> KeyEvent.KEYCODE_BACKSLASH;
            case ';' -> KeyEvent.KEYCODE_SEMICOLON;
            case '\'' -> KeyEvent.KEYCODE_APOSTROPHE;
            case ',' -> KeyEvent.KEYCODE_COMMA;
            case '.' -> KeyEvent.KEYCODE_PERIOD;
            case '/' -> KeyEvent.KEYCODE_SLASH;
            case '`' -> KeyEvent.KEYCODE_GRAVE;
            case 0x4000004f -> KeyEvent.KEYCODE_DPAD_RIGHT;
            case 0x40000050 -> KeyEvent.KEYCODE_DPAD_LEFT;
            case 0x40000051 -> KeyEvent.KEYCODE_DPAD_DOWN;
            case 0x40000052 -> KeyEvent.KEYCODE_DPAD_UP;
            case 0x4000004a -> KeyEvent.KEYCODE_MOVE_HOME;
            case 0x4000004d -> KeyEvent.KEYCODE_MOVE_END;
            case 0x4000004b -> KeyEvent.KEYCODE_PAGE_UP;
            case 0x4000004e -> KeyEvent.KEYCODE_PAGE_DOWN;
            case 0x400000e1, 0x400000e5 -> KeyEvent.KEYCODE_SHIFT_LEFT;
            case 0x400000e0, 0x400000e4 -> KeyEvent.KEYCODE_CTRL_LEFT;
            default -> KeyEvent.KEYCODE_UNKNOWN;
        };
    }

    /** SDL's modifiers (SDL_KMOD_*) as Android's meta state */
    static int meta(int mod) {
        int m = 0;
        if ((mod & 0x0003) != 0) m |= KeyEvent.META_SHIFT_ON;
        if ((mod & 0x00c0) != 0) m |= KeyEvent.META_CTRL_ON;
        if ((mod & 0x0300) != 0) m |= KeyEvent.META_ALT_ON;
        if ((mod & 0x2000) != 0) m |= KeyEvent.META_CAPS_LOCK_ON;
        return m;
    }

    private static native boolean nativeOpen(int width, int height, String title);
    private static native boolean nativeHasWindow();
    private static native void nativeResize(int width, int height);
    /**
     * A frame: its list, and its bitmaps (ID, width and height in info, three ints each) with the pixels to upload and
     * the operations to draw into each first (null for none), and the bitmaps the app let go.
     */
    private static native void nativePresent(int width, int height, byte[] list, int size, int[] info, int[][] pixels,
                                             byte[][] drawings, int[] freed);
    private static native int nativePoll(float[] events);
}
