// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.provider;

import android.content.ContentResolver;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import java.io.IOException;
import java.io.InputStream;
import java.util.Set;

/** The media provider's contract. The device's shared storage has no indexed media, so no thumbnail is found. */
public final class MediaStore {
    public static final String AUTHORITY = "media";
    public static final Uri AUTHORITY_URI = Uri.parse("content://media");
    public static final String VOLUME_INTERNAL = "internal", VOLUME_EXTERNAL = "external", VOLUME_EXTERNAL_PRIMARY = "external_primary";
    public static final String ACTION_IMAGE_CAPTURE = "android.media.action.IMAGE_CAPTURE",
        ACTION_VIDEO_CAPTURE = "android.media.action.VIDEO_CAPTURE", ACTION_PICK_IMAGES = "android.provider.action.PICK_IMAGES",
        EXTRA_OUTPUT = "output", EXTRA_PICK_IMAGES_MAX = "android.provider.extra.PICK_IMAGES_MAX";

    private MediaStore() {}

    public static Set<String> getExternalVolumeNames(Context context) { return Set.of(VOLUME_EXTERNAL_PRIMARY); }
    public static int getPickImagesMaxLimit() { return 100; }
    @Deprecated public static Uri setIncludePending(Uri uri) { return uri.buildUpon().appendQueryParameter("includePending", "1").build(); }

    private static Uri uri(String volume, String path) { return Uri.parse("content://media/" + volume + "/" + path); }

    public interface MediaColumns extends BaseColumns {
        String DATA = "_data", SIZE = "_size", DISPLAY_NAME = "_display_name", TITLE = "title", DATE_ADDED = "date_added",
            DATE_MODIFIED = "date_modified", MIME_TYPE = "mime_type", WIDTH = "width", HEIGHT = "height",
            RELATIVE_PATH = "relative_path", IS_PENDING = "is_pending", DURATION = "duration";
    }

    public static final class Files {
        public static Uri getContentUri(String volumeName) { return uri(volumeName, "file"); }
    }

    public static final class Audio {
        public static final class Media implements MediaColumns {
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL), EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);
            public static Uri getContentUri(String volumeName) { return uri(volumeName, "audio/media"); }
        }
    }

    public static final class Images {
        public static final class Media implements MediaColumns {
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL), EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);
            public static final String CONTENT_TYPE = "vnd.android.cursor.dir/image";
            public static Uri getContentUri(String volumeName) { return uri(volumeName, "images/media"); }

            @Deprecated
            public static Bitmap getBitmap(ContentResolver cr, Uri url) throws IOException {
                try (InputStream in = cr.openInputStream(url)) { return BitmapFactory.decodeStream(in); }
            }
        }

        public static class Thumbnails implements BaseColumns {
            public static final int MINI_KIND = 1, FULL_SCREEN_KIND = 2, MICRO_KIND = 3;
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL), EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);
            public static Uri getContentUri(String volumeName) { return uri(volumeName, "images/thumbnails"); }
            @Deprecated
            public static Bitmap getThumbnail(ContentResolver cr, long origId, int kind, BitmapFactory.Options options) { return null; }
        }
    }

    public static final class Video {
        public static final class Media implements MediaColumns {
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL), EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);
            public static Uri getContentUri(String volumeName) { return uri(volumeName, "video/media"); }
        }

        public static class Thumbnails implements BaseColumns {
            public static final int MINI_KIND = 1, FULL_SCREEN_KIND = 2, MICRO_KIND = 3;
            public static final Uri INTERNAL_CONTENT_URI = getContentUri(VOLUME_INTERNAL), EXTERNAL_CONTENT_URI = getContentUri(VOLUME_EXTERNAL);
            public static Uri getContentUri(String volumeName) { return uri(volumeName, "video/thumbnails"); }
            @Deprecated
            public static Bitmap getThumbnail(ContentResolver cr, long origId, int kind, BitmapFactory.Options options) { return null; }
        }
    }
}
