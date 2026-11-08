// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.slice;

import android.app.PendingIntent;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Bundle;
import java.util.ArrayList;
import java.util.List;

/** Content of an app for another surface (deprecated): a uri, a spec, hints and items. */
@Deprecated
public final class Slice implements android.os.Parcelable {
    public static final String HINT_TITLE = "title", HINT_LIST = "list", HINT_LIST_ITEM = "list_item", HINT_LARGE = "large",
        HINT_ACTIONS = "actions", HINT_SELECTED = "selected", HINT_NO_TINT = "no_tint", HINT_SHORTCUT = "shortcut",
        HINT_TOGGLE = "toggle", HINT_HORIZONTAL = "horizontal", HINT_PARTIAL = "partial", HINT_SEE_MORE = "see_more",
        HINT_KEYWORDS = "keywords", HINT_ERROR = "error", HINT_TTL = "ttl", HINT_LAST_UPDATED = "last_updated",
        HINT_PERMISSION_REQUEST = "permission_request", HINT_SUMMARY = "summary";
    public static final String SUBTYPE_COLOR = "color", SUBTYPE_CONTENT_DESCRIPTION = "content_description", SUBTYPE_MAX = "max",
        SUBTYPE_MESSAGE = "message", SUBTYPE_SOURCE = "source", SUBTYPE_TOGGLE = "toggle", SUBTYPE_VALUE = "value",
        SUBTYPE_LAYOUT_DIRECTION = "layout_direction", SUBTYPE_MILLIS = "millis", SUBTYPE_RANGE = "range";
    public static final String EXTRA_TOGGLE_STATE = "android.app.slice.extra.TOGGLE_STATE",
        EXTRA_RANGE_VALUE = "android.app.slice.extra.RANGE_VALUE";
    private final Uri uri;
    private final SliceSpec spec;
    private final List<String> hints;
    private final List<SliceItem> items;

    private Slice(Builder b) {
        uri = b.uri;
        spec = b.spec;
        hints = List.copyOf(b.hints);
        items = List.copyOf(b.items);
    }
    public Uri getUri() { return uri; }
    public SliceSpec getSpec() { return spec; }
    public List<String> getHints() { return hints; }
    public List<SliceItem> getItems() { return items; }
    public boolean isCallerNeeded() { return false; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }

    public static class Builder {
        private final Uri uri;
        private final SliceSpec spec;
        private final ArrayList<String> hints = new ArrayList<>();
        private final ArrayList<SliceItem> items = new ArrayList<>();

        public Builder(Uri uri, SliceSpec spec) { this.uri = uri; this.spec = spec; }
        /** A builder of a sub-slice, under a uri made from the parent's. */
        public Builder(Builder parent) {
            this(parent.uri.buildUpon().appendPath("_gen").appendPath(String.valueOf(parent.items.size())).build(), parent.spec);
        }
        public Builder setCallerNeeded(boolean callerNeeded) { return this; }
        public Builder addHints(List<String> hints) { this.hints.addAll(hints); return this; }
        private Builder add(Object value, Slice slice, String format, String subType, List<String> hints) {
            items.add(new SliceItem(value, slice, format, subType, hints));
            return this;
        }
        public Builder addSubSlice(Slice slice, String subType) { return add(null, slice, SliceItem.FORMAT_SLICE, subType, slice.getHints()); }
        public Builder addAction(PendingIntent action, Slice s, String subType) {
            return add(action, s, SliceItem.FORMAT_ACTION, subType, s != null ? s.getHints() : null);
        }
        public Builder addText(CharSequence text, String subType, List<String> hints) { return add(text, null, SliceItem.FORMAT_TEXT, subType, hints); }
        public Builder addIcon(Icon icon, String subType, List<String> hints) { return add(icon, null, SliceItem.FORMAT_IMAGE, subType, hints); }
        public Builder addRemoteInput(android.app.RemoteInput remoteInput, String subType, List<String> hints) {
            return add(remoteInput, null, SliceItem.FORMAT_REMOTE_INPUT, subType, hints);
        }
        public Builder addInt(int value, String subType, List<String> hints) { return add(value, null, SliceItem.FORMAT_INT, subType, hints); }
        public Builder addLong(long value, String subType, List<String> hints) { return add(value, null, SliceItem.FORMAT_LONG, subType, hints); }
        public Builder addBundle(Bundle bundle, String subType, List<String> hints) { return add(bundle, null, SliceItem.FORMAT_BUNDLE, subType, hints); }
        public Slice build() { return new Slice(this); }
    }
}
