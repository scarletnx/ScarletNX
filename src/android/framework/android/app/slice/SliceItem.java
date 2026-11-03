// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.slice;

import android.app.PendingIntent;
import android.graphics.drawable.Icon;
import android.os.Bundle;
import java.util.List;

/** One item of a slice (deprecated): a value of one format, a subtype and hints. */
@Deprecated
public final class SliceItem implements android.os.Parcelable {
    public static final String FORMAT_SLICE = "slice", FORMAT_TEXT = "text", FORMAT_IMAGE = "image", FORMAT_ACTION = "action",
        FORMAT_INT = "int", FORMAT_LONG = "long", FORMAT_REMOTE_INPUT = "input", FORMAT_BUNDLE = "bundle";
    private final Object value;
    private final Slice slice;
    private final String format, subType;
    private final List<String> hints;

    SliceItem(Object value, Slice slice, String format, String subType, List<String> hints) {
        this.value = value;
        this.slice = slice;
        this.format = format;
        this.subType = subType;
        this.hints = hints != null ? List.copyOf(hints) : List.of();
    }
    public String getFormat() { return format; }
    public String getSubType() { return subType; }
    public List<String> getHints() { return hints; }
    public boolean hasHint(String hint) { return hints.contains(hint); }
    public CharSequence getText() { return value instanceof CharSequence c ? c : null; }
    public Icon getIcon() { return value instanceof Icon i ? i : null; }
    public PendingIntent getAction() { return value instanceof PendingIntent p ? p : null; }
    public Bundle getBundle() { return value instanceof Bundle b ? b : null; }
    public android.app.RemoteInput getRemoteInput() { return value instanceof android.app.RemoteInput r ? r : null; }
    public int getInt() { return value instanceof Integer i ? i : 0; }
    public long getLong() { return value instanceof Long l ? l : 0; }
    public Slice getSlice() { return slice; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
