// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.content;

/** A component: its package and class names. */
public final class ComponentName implements android.os.Parcelable, Cloneable, Comparable<ComponentName> {
    private final String pkg, cls;

    public ComponentName(String pkg, String cls) {
        if (pkg == null) throw new NullPointerException("package name is null");
        if (cls == null) throw new NullPointerException("class name is null");
        this.pkg = pkg;
        this.cls = cls;
    }
    public ComponentName(Context pkg, String cls) { this(pkg.getPackageName(), cls); }
    public ComponentName(Context pkg, Class<?> cls) { this(pkg.getPackageName(), cls.getName()); }
    public static ComponentName createRelative(String pkg, String cls) { return new ComponentName(pkg, cls.startsWith(".") ? pkg + cls : cls); }
    public static ComponentName createRelative(Context pkg, String cls) { return createRelative(pkg.getPackageName(), cls); }
    public String getPackageName() { return pkg; }
    public String getClassName() { return cls; }
    public String getShortClassName() { return cls.startsWith(pkg + ".") ? cls.substring(pkg.length()) : cls; }
    public String flattenToString() { return pkg + "/" + cls; }
    public String flattenToShortString() { return pkg + "/" + getShortClassName(); }
    public String toShortString() { return "{" + flattenToShortString() + "}"; }
    public static ComponentName unflattenFromString(String str) {
        int sep = str.indexOf('/');
        if (sep < 0 || sep + 1 >= str.length()) return null;
        String p = str.substring(0, sep), c = str.substring(sep + 1);
        return new ComponentName(p, c.startsWith(".") ? p + c : c);
    }
    @Override public ComponentName clone() { return new ComponentName(pkg, cls); }
    @Override public boolean equals(Object o) { return o instanceof ComponentName c && c.pkg.equals(pkg) && c.cls.equals(cls); }
    @Override public int hashCode() { return pkg.hashCode() + cls.hashCode(); }
    @Override public int compareTo(ComponentName that) { int v = pkg.compareTo(that.pkg); return v != 0 ? v : cls.compareTo(that.cls); }
    @Override public String toString() { return "ComponentInfo{" + pkg + "/" + cls + "}"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeString(pkg); out.writeString(cls); }
    public static final android.os.Parcelable.Creator<ComponentName> CREATOR = new android.os.Parcelable.Creator<>() {
        @Override public ComponentName createFromParcel(android.os.Parcel in) { return new ComponentName(in.readString(), in.readString()); }
        @Override public ComponentName[] newArray(int size) { return new ComponentName[size]; }
    };
}
