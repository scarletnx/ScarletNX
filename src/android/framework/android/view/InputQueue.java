// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/**
 * A window's input events for native code (NativeActivity's AInputQueue): a window that a NativeActivity takes the
 * queue of sends the system's touches and keys here instead of to its views (ViewRootImpl), and the native code reads
 * them with AInputQueue_getEvent. Only the 32-bit native bridge has the queue yet (create gives null otherwise).
 */
public final class InputQueue {
    public interface Callback {
        void onInputQueueCreated(InputQueue queue);
        void onInputQueueDestroyed(InputQueue queue);
    }

    private final long handle;

    private InputQueue(long handle) { this.handle = handle; }

    /** a new queue, null when the native bridge has none */
    static InputQueue create() {
        long h = snx.android.nativebridge.NativeBridge.inputQueueNew();
        return h != 0 ? new InputQueue(h) : null;
    }
    /** the native AInputQueue's address, which the native callbacks receive */
    public long handle() { return handle; }

    /** the event queued for the native code: true, as the native code takes it from here */
    boolean send(InputEvent event) {
        if (event instanceof MotionEvent m) {
            int n = m.getPointerCount();
            int[] ids = new int[n], tools = new int[n];
            float[] axes = new float[9 * n];
            for (int i = 0; i < n; ++i) {
                ids[i] = m.getPointerId(i);
                tools[i] = m.getToolType(i);
                float[] a = {m.getX(i), m.getY(i), m.getPressure(i), m.getSize(i), m.getTouchMajor(i), m.getTouchMinor(i),
                    m.getToolMajor(i), m.getToolMinor(i), m.getOrientation(i)};
                System.arraycopy(a, 0, axes, 9 * i, 9);
            }
            snx.android.nativebridge.NativeBridge.inputQueueMotion(handle, m.getDeviceId(), m.getSource(), m.getAction(),
                m.getFlags(), m.getMetaState(), m.getButtonState(), m.getEdgeFlags(), m.getDownTime() * 1000000L,
                m.getEventTime() * 1000000L, m.getXPrecision(), m.getYPrecision(), ids, tools, axes);
            return true;
        }
        if (event instanceof KeyEvent k) {
            snx.android.nativebridge.NativeBridge.inputQueueKey(handle, k.getDeviceId(), k.getSource(), k.getAction(),
                k.getFlags(), k.getKeyCode(), k.getScanCode(), k.getMetaState(), k.getRepeatCount(),
                k.getDownTime() * 1000000L, k.getEventTime() * 1000000L);
            return true;
        }
        return false;
    }
}
