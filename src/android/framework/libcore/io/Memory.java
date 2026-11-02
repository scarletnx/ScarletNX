// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package libcore.io;

import java.nio.ByteOrder;

/**
 * ART's libcore.io.Memory, which native code and some libraries reach although it is hidden: reads and writes of
 * memory by address (the native bridge's addresses are host addresses) and of byte arrays in a byte order. A shim of
 * the ART internals that apps touch (docs/android/INTERFACES.md).
 */
public final class Memory {
    private static final sun.misc.Unsafe U;
    static {
        try {
            java.lang.reflect.Field f = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            U = (sun.misc.Unsafe) f.get(null);
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }
    private static final boolean BIG = ByteOrder.nativeOrder() == ByteOrder.BIG_ENDIAN;

    private Memory() {}

    public static byte peekByte(long address) { return U.getByte(address); }
    public static void pokeByte(long address, byte value) { U.putByte(address, value); }
    /** swap: the value's bytes in the other order than the host's */
    public static short peekShort(long address, boolean swap) { short v = U.getShort(address); return swap ? Short.reverseBytes(v) : v; }
    public static int peekInt(long address, boolean swap) { int v = U.getInt(address); return swap ? Integer.reverseBytes(v) : v; }
    public static long peekLong(long address, boolean swap) { long v = U.getLong(address); return swap ? Long.reverseBytes(v) : v; }
    public static void pokeShort(long address, short value, boolean swap) { U.putShort(address, swap ? Short.reverseBytes(value) : value); }
    public static void pokeInt(long address, int value, boolean swap) { U.putInt(address, swap ? Integer.reverseBytes(value) : value); }
    public static void pokeLong(long address, long value, boolean swap) { U.putLong(address, swap ? Long.reverseBytes(value) : value); }
    public static void peekByteArray(long address, byte[] dst, int offset, int count) {
        for (int i = 0; i < count; ++i) dst[offset + i] = U.getByte(address + i);
    }
    public static void pokeByteArray(long address, byte[] src, int offset, int count) {
        for (int i = 0; i < count; ++i) U.putByte(address + i, src[offset + i]);
    }

    private static boolean swapped(ByteOrder order) { return (order == ByteOrder.BIG_ENDIAN) != BIG; }
    public static short peekShort(byte[] src, int offset, ByteOrder order) {
        return order == ByteOrder.BIG_ENDIAN ? (short) ((src[offset] << 8) | (src[offset + 1] & 0xff))
                                             : (short) ((src[offset + 1] << 8) | (src[offset] & 0xff));
    }
    public static int peekInt(byte[] src, int offset, ByteOrder order) {
        int v = 0;
        for (int i = 0; i < 4; ++i) v |= (src[offset + (order == ByteOrder.BIG_ENDIAN ? 3 - i : i)] & 0xff) << (8 * i);
        return v;
    }
    public static long peekLong(byte[] src, int offset, ByteOrder order) {
        long v = 0;
        for (int i = 0; i < 8; ++i) v |= (src[offset + (order == ByteOrder.BIG_ENDIAN ? 7 - i : i)] & 0xffL) << (8 * i);
        return v;
    }
    public static void pokeShort(byte[] dst, int offset, short value, ByteOrder order) {
        for (int i = 0; i < 2; ++i) dst[offset + (order == ByteOrder.BIG_ENDIAN ? 1 - i : i)] = (byte) (value >> (8 * i));
    }
    public static void pokeInt(byte[] dst, int offset, int value, ByteOrder order) {
        for (int i = 0; i < 4; ++i) dst[offset + (order == ByteOrder.BIG_ENDIAN ? 3 - i : i)] = (byte) (value >> (8 * i));
    }
    public static void pokeLong(byte[] dst, int offset, long value, ByteOrder order) {
        for (int i = 0; i < 8; ++i) dst[offset + (order == ByteOrder.BIG_ENDIAN ? 7 - i : i)] = (byte) (value >> (8 * i));
    }
    /** byteCount bytes between two arrays (of any primitive type) at byte offsets */
    public static void memmove(Object dstObject, int dstOffset, Object srcObject, int srcOffset, long byteCount) {
        long db = U.arrayBaseOffset(dstObject.getClass()), sb = U.arrayBaseOffset(srcObject.getClass());
        U.copyMemory(srcObject, sb + srcOffset, dstObject, db + dstOffset, byteCount);
    }
    @SuppressWarnings("unused")
    private static boolean unused(ByteOrder order) { return swapped(order); }
}
