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
 * SurfaceFlinger and the input dispatcher in docs/android/INTERFACES.md. Without a display the renderer draws into an
 * image of its own (SNX_ANDROID_SHOT saves one). SNX_ANDROID_HEADLESS draws nothing at all.
 *
 * The main looper waits in short steps and reads the window's events between them, since SDL's event queue belongs
 * to the thread that made the window, and that is the main thread.
 */
public final class Window {
    private static boolean open, failed;
    private static final boolean headless = System.getenv("SNX_ANDROID_HEADLESS") != null;
    private static final float[] events = new float[8 * 64];
    private static long downTime;
    private static final int POLL_MS = 4;

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
            nativePresent(frame.width, frame.height, frame.bytes(), frame.size(),
                frame.bitmaps().toArray(new android.graphics.Bitmap[0]));
        } catch (UnsatisfiedLinkError e) {
            failed = true;
            android.util.Log.w("snx", "no renderer in this runtime: running headless");
        }
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
                    if (root != null) root.resize(w, h);
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
    private static native void nativePresent(int width, int height, byte[] list, int size, android.graphics.Bitmap[] bitmaps);
    private static native int nativePoll(float[] events);
}
