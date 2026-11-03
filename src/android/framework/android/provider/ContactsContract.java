// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.provider;

import android.content.ContentResolver;
import android.net.Uri;
import java.io.InputStream;

/** The contacts provider's contract. The device has no contacts, so no contact is found. */
public final class ContactsContract {
    public static final String AUTHORITY = "com.android.contacts";
    public static final Uri AUTHORITY_URI = Uri.parse("content://com.android.contacts");

    private ContactsContract() {}

    public static class Contacts implements BaseColumns {
        public static final Uri CONTENT_URI = Uri.withAppendedPath(AUTHORITY_URI, "contacts");
        public static final Uri CONTENT_LOOKUP_URI = Uri.withAppendedPath(CONTENT_URI, "lookup");
        public static final Uri CONTENT_FILTER_URI = Uri.withAppendedPath(CONTENT_URI, "filter");
        public static final String CONTENT_TYPE = "vnd.android.cursor.dir/contact", CONTENT_ITEM_TYPE = "vnd.android.cursor.item/contact";
        public static final String DISPLAY_NAME = "display_name", LOOKUP_KEY = "lookup", PHOTO_ID = "photo_id",
            PHOTO_URI = "photo_uri", PHOTO_THUMBNAIL_URI = "photo_thumb_uri", HAS_PHONE_NUMBER = "has_phone_number";

        private Contacts() {}

        public static Uri lookupContact(ContentResolver resolver, Uri lookupUri) { return null; }
        public static InputStream openContactPhotoInputStream(ContentResolver cr, Uri contactUri, boolean preferHighres) { return null; }
        public static InputStream openContactPhotoInputStream(ContentResolver cr, Uri contactUri) { return null; }
    }
}
