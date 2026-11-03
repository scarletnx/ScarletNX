// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.Set;

/** The metadata of the content that a session plays: text, numbers, a rating and bitmaps, by key. */
public final class MediaMetadata implements Parcelable {
    public static final String METADATA_KEY_TITLE = "android.media.metadata.TITLE", METADATA_KEY_ARTIST = "android.media.metadata.ARTIST",
        METADATA_KEY_DURATION = "android.media.metadata.DURATION", METADATA_KEY_ALBUM = "android.media.metadata.ALBUM",
        METADATA_KEY_AUTHOR = "android.media.metadata.AUTHOR", METADATA_KEY_WRITER = "android.media.metadata.WRITER",
        METADATA_KEY_COMPOSER = "android.media.metadata.COMPOSER", METADATA_KEY_COMPILATION = "android.media.metadata.COMPILATION",
        METADATA_KEY_DATE = "android.media.metadata.DATE", METADATA_KEY_YEAR = "android.media.metadata.YEAR",
        METADATA_KEY_GENRE = "android.media.metadata.GENRE", METADATA_KEY_TRACK_NUMBER = "android.media.metadata.TRACK_NUMBER",
        METADATA_KEY_NUM_TRACKS = "android.media.metadata.NUM_TRACKS", METADATA_KEY_DISC_NUMBER = "android.media.metadata.DISC_NUMBER",
        METADATA_KEY_ALBUM_ARTIST = "android.media.metadata.ALBUM_ARTIST", METADATA_KEY_ART = "android.media.metadata.ART",
        METADATA_KEY_ART_URI = "android.media.metadata.ART_URI", METADATA_KEY_ALBUM_ART = "android.media.metadata.ALBUM_ART",
        METADATA_KEY_ALBUM_ART_URI = "android.media.metadata.ALBUM_ART_URI", METADATA_KEY_USER_RATING = "android.media.metadata.USER_RATING",
        METADATA_KEY_RATING = "android.media.metadata.RATING", METADATA_KEY_DISPLAY_TITLE = "android.media.metadata.DISPLAY_TITLE",
        METADATA_KEY_DISPLAY_SUBTITLE = "android.media.metadata.DISPLAY_SUBTITLE",
        METADATA_KEY_DISPLAY_DESCRIPTION = "android.media.metadata.DISPLAY_DESCRIPTION",
        METADATA_KEY_DISPLAY_ICON = "android.media.metadata.DISPLAY_ICON", METADATA_KEY_DISPLAY_ICON_URI = "android.media.metadata.DISPLAY_ICON_URI",
        METADATA_KEY_MEDIA_ID = "android.media.metadata.MEDIA_ID", METADATA_KEY_MEDIA_URI = "android.media.metadata.MEDIA_URI",
        METADATA_KEY_BT_FOLDER_TYPE = "android.media.metadata.BT_FOLDER_TYPE";

    private final Bundle bundle;

    private MediaMetadata(Bundle bundle) { this.bundle = new Bundle(bundle); }

    public boolean containsKey(String key) { return bundle.containsKey(key); }
    public CharSequence getText(String key) { return bundle.getCharSequence(key); }
    public String getString(String key) { CharSequence t = getText(key); return t != null ? t.toString() : null; }
    public long getLong(String key) { return bundle.getLong(key, 0); }
    public Rating getRating(String key) { return (Rating) bundle.getParcelable(key); }
    public Bitmap getBitmap(String key) { return (Bitmap) bundle.getParcelable(key); }
    public int size() { return bundle.size(); }
    public Set<String> keySet() { return bundle.keySet(); }

    public MediaDescription getDescription() {
        MediaDescription.Builder b = new MediaDescription.Builder().setMediaId(getString(METADATA_KEY_MEDIA_ID));
        CharSequence title = getText(METADATA_KEY_DISPLAY_TITLE);
        if (title != null) {
            b.setTitle(title).setSubtitle(getText(METADATA_KEY_DISPLAY_SUBTITLE)).setDescription(getText(METADATA_KEY_DISPLAY_DESCRIPTION));
        } else {
            b.setTitle(getText(METADATA_KEY_TITLE)).setSubtitle(getText(METADATA_KEY_ARTIST)).setDescription(getText(METADATA_KEY_ALBUM));
        }
        Bitmap icon = getBitmap(METADATA_KEY_DISPLAY_ICON);
        if (icon == null) icon = getBitmap(METADATA_KEY_ART);
        if (icon == null) icon = getBitmap(METADATA_KEY_ALBUM_ART);
        String iconUri = getString(METADATA_KEY_DISPLAY_ICON_URI);
        if (iconUri == null) iconUri = getString(METADATA_KEY_ART_URI);
        if (iconUri == null) iconUri = getString(METADATA_KEY_ALBUM_ART_URI);
        String mediaUri = getString(METADATA_KEY_MEDIA_URI);
        return b.setIconBitmap(icon).setIconUri(iconUri != null ? Uri.parse(iconUri) : null)
            .setMediaUri(mediaUri != null ? Uri.parse(mediaUri) : null).build();
    }

    @Override public boolean equals(Object o) {
        if (!(o instanceof MediaMetadata m) || m.size() != size()) return false;
        for (String k : keySet()) if (!java.util.Objects.equals(bundle.get(k), m.bundle.get(k))) return false;
        return true;
    }
    @Override public int hashCode() { return keySet().hashCode(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeBundle(bundle); }
    public static final Parcelable.Creator<MediaMetadata> CREATOR = new Parcelable.Creator<>() {
        @Override public MediaMetadata createFromParcel(Parcel in) { return new MediaMetadata(in.readBundle()); }
        @Override public MediaMetadata[] newArray(int size) { return new MediaMetadata[size]; }
    };

    public static final class Builder {
        private final Bundle bundle;

        public Builder() { bundle = new Bundle(); }
        public Builder(MediaMetadata source) { bundle = new Bundle(source.bundle); }

        public Builder putText(String key, CharSequence value) { bundle.putCharSequence(key, value); return this; }
        public Builder putString(String key, String value) { bundle.putCharSequence(key, value); return this; }
        public Builder putLong(String key, long value) { bundle.putLong(key, value); return this; }
        public Builder putRating(String key, Rating value) { bundle.putParcelable(key, value); return this; }
        public Builder putBitmap(String key, Bitmap value) { bundle.putParcelable(key, value); return this; }
        public MediaMetadata build() { return new MediaMetadata(bundle); }
    }
}
