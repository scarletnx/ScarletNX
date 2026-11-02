// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.io.Serializable;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * A container of flattened values: libbinder's Parcel as a little-endian byte buffer. Binders and the live objects
 * a value can't flatten stay in a table beside the bytes, by index: everything is in one process. Strings are as
 * Android writes them, a length and UTF-16 units with a terminator, padded to four bytes.
 */
public final class Parcel {
    private ByteBuffer data = ByteBuffer.allocate(256).order(ByteOrder.LITTLE_ENDIAN);
    private int size;
    private final ArrayList<Object> objects = new ArrayList<>();

    private Parcel() {}

    public static Parcel obtain() { return new Parcel(); }
    public static Parcel obtain(IBinder binder) { return new Parcel(); }
    public void recycle() {}

    public int dataSize() { return size; }
    public int dataAvail() { return size - data.position(); }
    public int dataPosition() { return data.position(); }
    public int dataCapacity() { return data.capacity(); }
    public void setDataSize(int n) { ensure(n); size = n; if (data.position() > n) data.position(n); }
    public void setDataPosition(int pos) { data.position(Math.min(pos, data.capacity())); }
    public void setDataCapacity(int n) { ensure(n); }
    public boolean hasFileDescriptors() { return false; }

    public byte[] marshall() {
        byte[] out = new byte[size];
        System.arraycopy(data.array(), 0, out, 0, size);
        return out;
    }

    public void unmarshall(byte[] bytes, int offset, int length) {
        data = ByteBuffer.allocate(Math.max(length, 16)).order(ByteOrder.LITTLE_ENDIAN);
        data.put(bytes, offset, length);
        size = length;
        data.position(0);
    }

    public void appendFrom(Parcel parcel, int offset, int length) {
        ensure(data.position() + length);
        data.put(parcel.data.array(), offset, length);
        grow();
    }

    private void ensure(int need) {
        if (need <= data.capacity()) return;
        ByteBuffer bigger = ByteBuffer.allocate(Math.max(need, data.capacity() * 2)).order(ByteOrder.LITTLE_ENDIAN);
        int pos = data.position();
        bigger.put(data.array(), 0, Math.max(size, pos));
        bigger.position(pos);
        data = bigger;
    }

    private void grow() { if (data.position() > size) size = data.position(); }
    private void pad() { while ((data.position() & 3) != 0) { ensure(data.position() + 1); data.put((byte) 0); } grow(); }

    public void writeInterfaceToken(String name) { writeInt(0); writeString(name); }
    public void enforceInterface(String name) {
        readInt();
        String token = readString();
        if (name != null && !name.equals(token)) throw new SecurityException("Binder invocation to an incorrect interface");
    }
    public void writeNoException() { writeInt(0); }
    public void readException() {
        int code = readInt();
        if (code != 0) throw new RuntimeException("snx: Parcel exception " + code + ": " + readString());
    }

    public void writeByte(byte v) { writeInt(v); }
    public void writeBoolean(boolean v) { writeInt(v ? 1 : 0); }
    public void writeInt(int v) { ensure(data.position() + 4); data.putInt(v); grow(); }
    public void writeLong(long v) { ensure(data.position() + 8); data.putLong(v); grow(); }
    public void writeFloat(float v) { ensure(data.position() + 4); data.putFloat(v); grow(); }
    public void writeDouble(double v) { ensure(data.position() + 8); data.putDouble(v); grow(); }

    public void writeString(String s) {
        if (s == null) { writeInt(-1); return; }
        writeInt(s.length());
        ensure(data.position() + 2 * s.length() + 2);
        for (int i = 0; i < s.length(); ++i) data.putChar(s.charAt(i));
        data.putChar((char) 0);
        pad();
    }
    public void writeString8(String s) { writeString(s); }
    public void writeString16(String s) { writeString(s); }
    public void writeCharSequence(CharSequence s) { writeString(s != null ? s.toString() : null); }

    public void writeByteArray(byte[] b) { writeByteArray(b, 0, b != null ? b.length : 0); }
    public void writeByteArray(byte[] b, int offset, int len) {
        if (b == null) { writeInt(-1); return; }
        writeInt(len);
        ensure(data.position() + len);
        data.put(b, offset, len);
        pad();
    }
    public void writeBlob(byte[] b) { writeByteArray(b); }
    public void writeIntArray(int[] a) { if (a == null) { writeInt(-1); return; } writeInt(a.length); for (int v : a) writeInt(v); }
    public void writeLongArray(long[] a) { if (a == null) { writeInt(-1); return; } writeInt(a.length); for (long v : a) writeLong(v); }
    public void writeFloatArray(float[] a) { if (a == null) { writeInt(-1); return; } writeInt(a.length); for (float v : a) writeFloat(v); }
    public void writeStringArray(String[] a) { if (a == null) { writeInt(-1); return; } writeInt(a.length); for (String v : a) writeString(v); }
    public void writeStringList(List<String> list) { if (list == null) { writeInt(-1); return; } writeInt(list.size()); for (String v : list) writeString(v); }

    /** A live object: its index in the table beside the bytes. */
    private void writeObject(Object o) { if (o == null) { writeInt(-1); return; } objects.add(o); writeInt(objects.size() - 1); }
    private Object readObject() { int i = readInt(); return i >= 0 && i < objects.size() ? objects.get(i) : null; }

    public void writeStrongBinder(IBinder b) { writeObject(b); }
    public void writeStrongInterface(IInterface i) { writeStrongBinder(i != null ? i.asBinder() : null); }
    public void writeBundle(Bundle b) { writeObject(b != null ? new Bundle(b) : null); }
    public void writePersistableBundle(PersistableBundle b) { writeObject(b != null ? new PersistableBundle(b) : null); }
    public void writeSerializable(Serializable s) { writeObject(s); }
    public void writeMap(Map<?, ?> m) { writeObject(m != null ? new HashMap<>(m) : null); }
    public void writeList(List<?> l) { writeObject(l != null ? new ArrayList<>(l) : null); }
    public void writeValue(Object v) { writeObject(v); }
    public void writeTypedObject(Parcelable p, int flags) { writeObject(p); }
    public void writeParcelable(Parcelable p, int flags) { writeObject(p); }
    public void writeTypedList(List<? extends Parcelable> l) { writeList(l); }
    public void writeTypedArray(Parcelable[] a, int flags) { writeObject(a); }
    public void writeParcelableArray(Parcelable[] a, int flags) { writeObject(a); }
    public void writeFileDescriptor(java.io.FileDescriptor fd) { writeObject(fd); }

    public byte readByte() { return (byte) readInt(); }
    public boolean readBoolean() { return readInt() != 0; }
    public int readInt() { return dataAvail() >= 4 ? data.getInt() : 0; }
    public long readLong() { return dataAvail() >= 8 ? data.getLong() : 0; }
    public float readFloat() { return dataAvail() >= 4 ? data.getFloat() : 0; }
    public double readDouble() { return dataAvail() >= 8 ? data.getDouble() : 0; }

    public String readString() {
        int n = readInt();
        if (n < 0 || n > dataAvail() / 2) return null;
        char[] c = new char[n];
        for (int i = 0; i < n; ++i) c[i] = data.getChar();
        data.getChar();
        while ((data.position() & 3) != 0 && data.position() < size) data.get();
        return new String(c);
    }
    public String readString8() { return readString(); }
    public String readString16() { return readString(); }
    public CharSequence readCharSequence() { return readString(); }

    public byte[] createByteArray() {
        int n = readInt();
        if (n < 0 || n > dataAvail()) return null;
        byte[] b = new byte[n];
        data.get(b);
        while ((data.position() & 3) != 0 && data.position() < size) data.get();
        return b;
    }
    public void readByteArray(byte[] dest) {
        byte[] b = createByteArray();
        if (b == null || b.length != dest.length) throw new RuntimeException("bad array lengths");
        System.arraycopy(b, 0, dest, 0, b.length);
    }
    public byte[] readBlob() { return createByteArray(); }
    public int[] createIntArray() { int n = readInt(); if (n < 0) return null; int[] a = new int[n]; for (int i = 0; i < n; ++i) a[i] = readInt(); return a; }
    public long[] createLongArray() { int n = readInt(); if (n < 0) return null; long[] a = new long[n]; for (int i = 0; i < n; ++i) a[i] = readLong(); return a; }
    public float[] createFloatArray() { int n = readInt(); if (n < 0) return null; float[] a = new float[n]; for (int i = 0; i < n; ++i) a[i] = readFloat(); return a; }
    public String[] createStringArray() { int n = readInt(); if (n < 0) return null; String[] a = new String[n]; for (int i = 0; i < n; ++i) a[i] = readString(); return a; }
    public ArrayList<String> createStringArrayList() { int n = readInt(); if (n < 0) return null; ArrayList<String> a = new ArrayList<>(n); for (int i = 0; i < n; ++i) a.add(readString()); return a; }
    public void readStringList(List<String> list) { list.clear(); int n = readInt(); for (int i = 0; i < n; ++i) list.add(readString()); }

    public IBinder readStrongBinder() { return (IBinder) readObject(); }
    public Bundle readBundle() { return (Bundle) readObject(); }
    public Bundle readBundle(ClassLoader loader) { return readBundle(); }
    public PersistableBundle readPersistableBundle() { return (PersistableBundle) readObject(); }
    public PersistableBundle readPersistableBundle(ClassLoader loader) { return readPersistableBundle(); }
    public Serializable readSerializable() { return (Serializable) readObject(); }
    public Object readValue(ClassLoader loader) { return readObject(); }
    @SuppressWarnings("unchecked")
    public <T extends Parcelable> T readParcelable(ClassLoader loader) { return (T) readObject(); }
    public <T> T readParcelable(ClassLoader loader, Class<T> clazz) { return clazz.cast(readObject()); }
    @SuppressWarnings("unchecked")
    public <T> T readTypedObject(Parcelable.Creator<T> c) { return (T) readObject(); }
    @SuppressWarnings("unchecked")
    public <T> ArrayList<T> createTypedArrayList(Parcelable.Creator<T> c) { return (ArrayList<T>) readObject(); }
    @SuppressWarnings("unchecked")
    public void readMap(Map outVal, ClassLoader loader) { Map<?, ?> m = (Map<?, ?>) readObject(); if (m != null) outVal.putAll(m); }
    @SuppressWarnings("unchecked")
    public void readList(List outVal, ClassLoader loader) { List<?> l = (List<?>) readObject(); if (l != null) outVal.addAll(l); }
    @SuppressWarnings("unchecked")
    public ArrayList readArrayList(ClassLoader loader) { return (ArrayList) readObject(); }
    public Parcelable[] readParcelableArray(ClassLoader loader) { return (Parcelable[]) readObject(); }
    public ParcelFileDescriptor readFileDescriptor() { return (ParcelFileDescriptor) readObject(); }
    public java.io.FileDescriptor readRawFileDescriptor() { return (java.io.FileDescriptor) readObject(); }

    /** A Parcelable's CREATOR, as Android finds it. */
    @SuppressWarnings("unchecked")
    public static <T> Parcelable.Creator<T> creator(Class<?> c) {
        try {
            Field f = c.getField("CREATOR");
            if (!Modifier.isStatic(f.getModifiers())) return null;
            return (Parcelable.Creator<T>) f.get(null);
        } catch (ReflectiveOperationException e) {
            return null;
        }
    }

    @Override
    public String toString() {
        return "Parcel{" + size + " bytes, " + objects.size() + " objects}";
    }
}
