// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

/** Rich content from an input method. */
public final class InputContentInfo implements android.os.Parcelable {
    private final android.net.Uri contentUri, linkUri;
    private final android.content.ClipDescription description;

    public InputContentInfo(android.net.Uri contentUri, android.content.ClipDescription description) { this(contentUri, description, null); }
    public InputContentInfo(android.net.Uri contentUri, android.content.ClipDescription description, android.net.Uri linkUri) {
        if (contentUri == null) throw new NullPointerException("contentUri");
        if (description == null) throw new NullPointerException("description");
        if (!"content".equals(contentUri.getScheme())) throw new java.security.InvalidParameterException("contentUri must have content scheme");
        this.contentUri = contentUri;
        this.description = description;
        this.linkUri = linkUri;
    }
    public android.net.Uri getContentUri() { return contentUri; }
    public android.content.ClipDescription getDescription() { return description; }
    public android.net.Uri getLinkUri() { return linkUri; }
    /** The app's own content needs no grant: no other process gives content here. */
    public void requestPermission() {}
    public void releasePermission() {}
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
