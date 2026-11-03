// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** What to show for a media item: its id, title, subtitle, description and icon. */
public class MediaDescription implements Parcelable {
    public static final long BT_FOLDER_TYPE_MIXED = 0, BT_FOLDER_TYPE_TITLES = 1, BT_FOLDER_TYPE_ALBUMS = 2,
        BT_FOLDER_TYPE_ARTISTS = 3, BT_FOLDER_TYPE_GENRES = 4, BT_FOLDER_TYPE_PLAYLISTS = 5, BT_FOLDER_TYPE_YEARS = 6;
    public static final String EXTRA_BT_FOLDER_TYPE = "android.media.extra.BT_FOLDER_TYPE";

    private final String mediaId;
    private final CharSequence title, subtitle, description;
    private final Bitmap icon;
    private final Uri iconUri, mediaUri;
    private final Bundle extras;

    private MediaDescription(String mediaId, CharSequence title, CharSequence subtitle, CharSequence description,
            Bitmap icon, Uri iconUri, Bundle extras, Uri mediaUri) {
        this.mediaId = mediaId; this.title = title; this.subtitle = subtitle; this.description = description;
        this.icon = icon; this.iconUri = iconUri; this.extras = extras; this.mediaUri = mediaUri;
    }

    public String getMediaId() { return mediaId; }
    public CharSequence getTitle() { return title; }
    public CharSequence getSubtitle() { return subtitle; }
    public CharSequence getDescription() { return description; }
    public Bitmap getIconBitmap() { return icon; }
    public Uri getIconUri() { return iconUri; }
    public Bundle getExtras() { return extras; }
    public Uri getMediaUri() { return mediaUri; }

    @Override public String toString() { return title + ", " + subtitle + ", " + description; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(mediaId);
        dest.writeCharSequence(title);
        dest.writeCharSequence(subtitle);
        dest.writeCharSequence(description);
        dest.writeValue(icon);
        dest.writeValue(iconUri);
        dest.writeBundle(extras);
        dest.writeValue(mediaUri);
    }
    public static final Parcelable.Creator<MediaDescription> CREATOR = new Parcelable.Creator<>() {
        @Override public MediaDescription createFromParcel(Parcel in) {
            return new MediaDescription(in.readString(), in.readCharSequence(), in.readCharSequence(), in.readCharSequence(),
                (Bitmap) in.readValue(null), (Uri) in.readValue(null), in.readBundle(), (Uri) in.readValue(null));
        }
        @Override public MediaDescription[] newArray(int size) { return new MediaDescription[size]; }
    };

    public static class Builder {
        private String mediaId;
        private CharSequence title, subtitle, description;
        private Bitmap icon;
        private Uri iconUri, mediaUri;
        private Bundle extras;

        public Builder() {}
        public Builder setMediaId(String mediaId) { this.mediaId = mediaId; return this; }
        public Builder setTitle(CharSequence title) { this.title = title; return this; }
        public Builder setSubtitle(CharSequence subtitle) { this.subtitle = subtitle; return this; }
        public Builder setDescription(CharSequence description) { this.description = description; return this; }
        public Builder setIconBitmap(Bitmap icon) { this.icon = icon; return this; }
        public Builder setIconUri(Uri iconUri) { this.iconUri = iconUri; return this; }
        public Builder setExtras(Bundle extras) { this.extras = extras; return this; }
        public Builder setMediaUri(Uri mediaUri) { this.mediaUri = mediaUri; return this; }
        public MediaDescription build() { return new MediaDescription(mediaId, title, subtitle, description, icon, iconUri, extras, mediaUri); }
    }
}
