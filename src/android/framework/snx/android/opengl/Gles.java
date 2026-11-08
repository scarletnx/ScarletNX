// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.opengl;

import java.nio.Buffer;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.DoubleBuffer;
import java.nio.LongBuffer;
import java.nio.ShortBuffer;

/**
 * The calls of android.opengl.GLES20: each one the runtime's GLES function of its name (src/android/bridge/hle_egl.c),
 * on the calling thread's current context, with its arguments as native code passes them. kinds gives each one's kind
 * in order: i an integer (of ints), f a float (of floats), p a pointer, s a string and S an array of one string (each of
 * objects, a pointer from its byte offset of offsets). A pointer is a direct buffer's memory or an array's.
 */
public final class Gles {
    private Gles() {}

    public static native long call(String name, String kinds, long[] ints, float[] floats, Object[] objects, int[] offsets);
    /** glGetString: the current context's string */
    public static native String string(int name);

    private static int shift(Buffer b) {
        if (b instanceof ByteBuffer) return 0;
        if (b instanceof ShortBuffer || b instanceof CharBuffer) return 1;
        if (b instanceof LongBuffer || b instanceof DoubleBuffer) return 3;
        return 2;
    }
    /** a buffer's memory from its position: the buffer itself when it is direct, else its array */
    public static Object base(Buffer b) {
        if (b == null) return null;
        if (b.isDirect()) return b;
        if (b.hasArray()) return b.array();
        throw new IllegalArgumentException("a buffer must be direct or have an array");
    }
    public static int offset(Buffer b) {
        return b == null ? 0 : ((b.isDirect() ? 0 : b.arrayOffset()) + b.position()) << shift(b);
    }
    /** an array from an element's offset */
    public static void checkArray(Object array, int offset) {
        if (array == null) throw new IllegalArgumentException("the array must not be null");
        if (offset < 0) throw new IllegalArgumentException("offset < 0");
    }
}
