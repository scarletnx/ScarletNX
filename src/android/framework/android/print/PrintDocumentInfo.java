// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2013 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.print;

import android.os.Parcel;
import android.os.Parcelable;

/** The description of a document to print: its name, page count and content type. */
public final class PrintDocumentInfo implements Parcelable {
    public static final int PAGE_COUNT_UNKNOWN = -1;
    public static final int CONTENT_TYPE_UNKNOWN = -1, CONTENT_TYPE_DOCUMENT = 0, CONTENT_TYPE_PHOTO = 1;
    private final String name;
    private final int pageCount, contentType;

    private PrintDocumentInfo(String name, int pageCount, int contentType) { this.name = name; this.pageCount = pageCount; this.contentType = contentType; }
    public String getName() { return name; }
    public int getPageCount() { return pageCount; }
    public int getContentType() { return contentType; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<PrintDocumentInfo> CREATOR = new Parcelable.Creator<>() {
        @Override public PrintDocumentInfo createFromParcel(Parcel in) { return (PrintDocumentInfo) in.readValue(null); }
        @Override public PrintDocumentInfo[] newArray(int size) { return new PrintDocumentInfo[size]; }
    };

    public static final class Builder {
        private final String name;
        private int pageCount = PAGE_COUNT_UNKNOWN, contentType = CONTENT_TYPE_DOCUMENT;

        public Builder(String name) {
            if (name == null || name.isEmpty()) throw new IllegalArgumentException("name cannot be empty");
            this.name = name;
        }
        public Builder setPageCount(int pageCount) {
            if (pageCount < 0 && pageCount != PAGE_COUNT_UNKNOWN) throw new IllegalArgumentException("pageCount must be greater than or equal to zero or PAGE_COUNT_UNKNOWN");
            this.pageCount = pageCount; return this;
        }
        public Builder setContentType(int type) { contentType = type; return this; }
        public PrintDocumentInfo build() { return new PrintDocumentInfo(name, pageCount, contentType); }
    }
}
