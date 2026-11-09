// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import android.util.Size;
import android.util.SizeF;
import android.util.SparseArray;
import java.util.ArrayList;

/** Keyed values that go with an Intent or a saved state. In one process nothing is marshalled. */
public final class Bundle extends BaseBundle implements Cloneable, Parcelable {
    public static final Bundle EMPTY = new Bundle();

    public Bundle() {}
    public Bundle(int capacity) { super(capacity); }
    public Bundle(Bundle b) { super(b); }
    public Bundle(PersistableBundle b) { super(b); }
    public Bundle(ClassLoader loader) { classLoader = loader; }

    public static Bundle forPair(String key, String value) {
        Bundle b = new Bundle(1);
        b.putString(key, value);
        return b;
    }

    @Override public Object clone() { return new Bundle(this); }
    public Bundle deepCopy() { return new Bundle(this); }
    public void setClassLoader(ClassLoader loader) { classLoader = loader; }
    public ClassLoader getClassLoader() { return classLoader; }
    public boolean hasFileDescriptors() { return false; }
    public void putAll(Bundle bundle) { map.putAll(bundle.map); }

    public void putBundle(String key, Bundle value) { map.put(key, value); }
    public void putBinder(String key, IBinder value) { map.put(key, value); }
    public void putParcelable(String key, Parcelable value) { map.put(key, value); }
    public void putParcelableArray(String key, Parcelable[] value) { map.put(key, value); }
    public void putParcelableArrayList(String key, ArrayList<? extends Parcelable> value) { map.put(key, value); }
    public void putSparseParcelableArray(String key, SparseArray<? extends Parcelable> value) { map.put(key, value); }
    public void putSize(String key, Size value) { map.put(key, value); }
    public void putSizeF(String key, SizeF value) { map.put(key, value); }
    public void putByteArray(String key, byte[] value) { map.put(key, value); }
    public void putShortArray(String key, short[] value) { map.put(key, value); }
    public void putCharArray(String key, char[] value) { map.put(key, value); }
    public void putFloatArray(String key, float[] value) { map.put(key, value); }
    public void putCharSequenceArray(String key, CharSequence[] value) { map.put(key, value); }
    public void putCharSequenceArrayList(String key, ArrayList<CharSequence> value) { map.put(key, value); }

    public Bundle getBundle(String key) { return typed(key, Bundle.class); }
    public IBinder getBinder(String key) { return typed(key, IBinder.class); }
    @SuppressWarnings("unchecked")
    public <T extends Parcelable> T getParcelable(String key) { return (T) typed(key, Parcelable.class); }
    public <T> T getParcelable(String key, Class<T> clazz) { return typed(key, clazz); }
    public Parcelable[] getParcelableArray(String key) { return typed(key, Parcelable[].class); }
    public <T> T[] getParcelableArray(String key, Class<T> clazz) {
        Parcelable[] a = getParcelableArray(key);
        if (a == null) return null;
        @SuppressWarnings("unchecked")
        T[] out = (T[]) java.lang.reflect.Array.newInstance(clazz, a.length);
        for (int i = 0; i < a.length; ++i) out[i] = clazz.cast(a[i]);
        return out;
    }
    @SuppressWarnings("unchecked")
    public <T extends Parcelable> ArrayList<T> getParcelableArrayList(String key) { return typed(key, ArrayList.class); }
    @SuppressWarnings("unchecked")
    public <T> ArrayList<T> getParcelableArrayList(String key, Class<? extends T> clazz) { return typed(key, ArrayList.class); }
    @SuppressWarnings("unchecked")
    public <T extends Parcelable> SparseArray<T> getSparseParcelableArray(String key) { return typed(key, SparseArray.class); }
    @SuppressWarnings("unchecked")
    public <T> SparseArray<T> getSparseParcelableArray(String key, Class<? extends T> clazz) { return typed(key, SparseArray.class); }
    public Size getSize(String key) { return typed(key, Size.class); }
    public SizeF getSizeF(String key) { return typed(key, SizeF.class); }
    public byte[] getByteArray(String key) { return typed(key, byte[].class); }
    public short[] getShortArray(String key) { return typed(key, short[].class); }
    public char[] getCharArray(String key) { return typed(key, char[].class); }
    public float[] getFloatArray(String key) { return typed(key, float[].class); }
    public CharSequence[] getCharSequenceArray(String key) { return typed(key, CharSequence[].class); }
    @SuppressWarnings("unchecked")
    public ArrayList<CharSequence> getCharSequenceArrayList(String key) { return typed(key, ArrayList.class); }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeBundle(this); }

    public static final Parcelable.Creator<Bundle> CREATOR = new Parcelable.Creator<>() {
        @Override public Bundle createFromParcel(Parcel in) { return in.readBundle(); }
        @Override public Bundle[] newArray(int size) { return new Bundle[size]; }
    };
}
