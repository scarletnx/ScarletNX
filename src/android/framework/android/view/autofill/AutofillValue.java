// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.autofill;

/** A value autofill puts in a view. */
public final class AutofillValue implements android.os.Parcelable {
    private final int type;
    private final Object value;
    private AutofillValue(int type, Object value) { this.type = type; this.value = value; }
    public static AutofillValue forText(CharSequence value) { return value == null ? null : new AutofillValue(1, value); }
    public static AutofillValue forToggle(boolean value) { return new AutofillValue(2, value); }
    public static AutofillValue forList(int value) { return new AutofillValue(3, value); }
    public static AutofillValue forDate(long value) { return new AutofillValue(4, value); }
    public CharSequence getTextValue() { return (CharSequence) value; }
    public boolean getToggleValue() { return (Boolean) value; }
    public int getListValue() { return (Integer) value; }
    public long getDateValue() { return (Long) value; }
    public boolean isText() { return type == 1; }
    public boolean isToggle() { return type == 2; }
    public boolean isList() { return type == 3; }
    public boolean isDate() { return type == 4; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
