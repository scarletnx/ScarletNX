// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (c) 2010, The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.content;

/** A clip's label and MIME types. */
public class ClipDescription implements android.os.Parcelable {
    public static final String MIMETYPE_TEXT_PLAIN = "text/plain", MIMETYPE_TEXT_HTML = "text/html",
        MIMETYPE_TEXT_URILIST = "text/uri-list", MIMETYPE_TEXT_INTENT = "text/vnd.android.intent", MIMETYPE_UNKNOWN = "application/octet-stream";
    private final CharSequence label;
    private final String[] mimeTypes;

    public ClipDescription(CharSequence label, String[] mimeTypes) { this.label = label; this.mimeTypes = mimeTypes; }
    public ClipDescription(ClipDescription o) { label = o.label; mimeTypes = o.mimeTypes; }
    public CharSequence getLabel() { return label; }
    public int getMimeTypeCount() { return mimeTypes.length; }
    public String getMimeType(int index) { return mimeTypes[index]; }
    public boolean hasMimeType(String mimeType) {
        for (String m : mimeTypes) if (compareMimeTypes(m, mimeType)) return true;
        return false;
    }
    public static boolean compareMimeTypes(String concreteType, String desiredType) {
        if (desiredType.equals("*/*") || concreteType.equals(desiredType)) return true;
        return desiredType.endsWith("/*") && concreteType.startsWith(desiredType.substring(0, desiredType.length() - 1));
    }
    public android.os.PersistableBundle getExtras() { return null; }
    public void setExtras(android.os.PersistableBundle extras) {}
    public long getTimestamp() { return 0; }
    public boolean isStyledText() { return false; }
    public static final int CLASSIFICATION_NOT_COMPLETE = 1, CLASSIFICATION_NOT_PERFORMED = 2, CLASSIFICATION_COMPLETE = 3;
    /** No text classifier runs here, so no clip is classified. */
    public int getClassificationStatus() { return CLASSIFICATION_NOT_PERFORMED; }
    public float getConfidenceScore(String entity) {
        throw new IllegalStateException("Classification not complete");
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
