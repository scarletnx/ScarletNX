// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.util.Arrays;
import java.util.Locale;

/** An ordered list of locales, the user's preference first. */
public final class LocaleList implements Parcelable {
    private static final LocaleList EMPTY = new LocaleList();
    private final Locale[] list;

    public LocaleList(Locale... list) { this.list = list.clone(); }

    public static LocaleList getEmptyLocaleList() { return EMPTY; }
    public static LocaleList getDefault() { return new LocaleList(Locale.getDefault()); }
    public static LocaleList getAdjustedDefault() { return getDefault(); }
    public static void setDefault(LocaleList locales) { if (locales.size() > 0) Locale.setDefault(locales.get(0)); }
    public static LocaleList forLanguageTags(String tags) {
        if (tags == null || tags.isEmpty()) return EMPTY;
        String[] parts = tags.split(",");
        Locale[] l = new Locale[parts.length];
        for (int i = 0; i < parts.length; ++i) l[i] = Locale.forLanguageTag(parts[i]);
        return new LocaleList(l);
    }

    public Locale get(int index) { return index >= 0 && index < list.length ? list[index] : null; }
    public boolean isEmpty() { return list.length == 0; }
    public int size() { return list.length; }
    public int indexOf(Locale locale) { return Arrays.asList(list).indexOf(locale); }
    public Locale getFirstMatch(String[] supported) { return list.length > 0 ? list[0] : null; }
    public String toLanguageTags() {
        StringBuilder b = new StringBuilder();
        for (Locale l : list) b.append(b.length() > 0 ? "," : "").append(l.toLanguageTag());
        return b.toString();
    }

    @Override public boolean equals(Object o) { return o instanceof LocaleList l && Arrays.equals(list, l.list); }
    @Override public int hashCode() { return Arrays.hashCode(list); }
    @Override public String toString() { return Arrays.toString(list); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeString(toLanguageTags()); }
    public static final Parcelable.Creator<LocaleList> CREATOR = new Parcelable.Creator<>() {
        @Override public LocaleList createFromParcel(Parcel in) { return forLanguageTags(in.readString()); }
        @Override public LocaleList[] newArray(int size) { return new LocaleList[size]; }
    };
}
