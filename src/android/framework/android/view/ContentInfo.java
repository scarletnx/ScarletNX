// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.ClipData;
import android.net.Uri;
import android.os.Bundle;

/** Content that a view receives, with where it came from. */
public final class ContentInfo implements android.os.Parcelable {
    public static final int SOURCE_APP = 0, SOURCE_CLIPBOARD = 1, SOURCE_INPUT_METHOD = 2, SOURCE_DRAG_AND_DROP = 3,
        SOURCE_AUTOFILL = 4, SOURCE_PROCESS_TEXT = 5;
    public static final int FLAG_CONVERT_TO_PLAIN_TEXT = 1;
    private final ClipData clip;
    private final int source, flags;
    private final Uri linkUri;
    private final Bundle extras;

    private ContentInfo(Builder b) { clip = b.clip; source = b.source; flags = b.flags; linkUri = b.linkUri; extras = b.extras; }
    public ClipData getClip() { return clip; }
    public int getSource() { return source; }
    public int getFlags() { return flags; }
    public Uri getLinkUri() { return linkUri; }
    public Bundle getExtras() { return extras; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
    @Override public String toString() { return "ContentInfo{clip=" + clip + ", source=" + source + ", flags=" + flags + "}"; }

    public static final class Builder {
        private ClipData clip;
        private int source, flags;
        private Uri linkUri;
        private Bundle extras;

        public Builder(ContentInfo other) { clip = other.clip; source = other.source; flags = other.flags; linkUri = other.linkUri; extras = other.extras; }
        public Builder(ClipData clip, int source) { this.clip = clip; this.source = source; }
        public Builder setClip(ClipData clip) { this.clip = clip; return this; }
        public Builder setSource(int source) { this.source = source; return this; }
        public Builder setFlags(int flags) { this.flags = flags; return this; }
        public Builder setLinkUri(Uri linkUri) { this.linkUri = linkUri; return this; }
        public Builder setExtras(Bundle extras) { this.extras = extras; return this; }
        public ContentInfo build() {
            if (clip == null) throw new NullPointerException("clip");
            if (source < SOURCE_APP || source > SOURCE_PROCESS_TEXT) throw new IllegalArgumentException("source is out of range");
            return new ContentInfo(this);
        }
    }
}
