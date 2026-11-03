// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import java.util.LinkedHashMap;

/**
 * The download service: a device without a download provider. Each request is accepted and fails at once with
 * ERROR_UNKNOWN, and ACTION_DOWNLOAD_COMPLETE goes to the app as Android sends it for a failed download.
 */
public class DownloadManager {
    public static final String COLUMN_ID = "_id", COLUMN_TITLE = "title", COLUMN_DESCRIPTION = "description", COLUMN_URI = "uri",
        COLUMN_MEDIA_TYPE = "media_type", COLUMN_TOTAL_SIZE_BYTES = "total_size", COLUMN_LOCAL_URI = "local_uri",
        COLUMN_STATUS = "status", COLUMN_REASON = "reason", COLUMN_BYTES_DOWNLOADED_SO_FAR = "bytes_so_far",
        COLUMN_LAST_MODIFIED_TIMESTAMP = "last_modified_timestamp", COLUMN_MEDIAPROVIDER_URI = "mediaprovider_uri",
        COLUMN_LOCAL_FILENAME = "local_filename";
    public static final int STATUS_PENDING = 1, STATUS_RUNNING = 2, STATUS_PAUSED = 4, STATUS_SUCCESSFUL = 8, STATUS_FAILED = 16;
    public static final int ERROR_UNKNOWN = 1000, ERROR_FILE_ERROR = 1001, ERROR_UNHANDLED_HTTP_CODE = 1002, ERROR_HTTP_DATA_ERROR = 1004,
        ERROR_TOO_MANY_REDIRECTS = 1005, ERROR_INSUFFICIENT_SPACE = 1006, ERROR_DEVICE_NOT_FOUND = 1007, ERROR_CANNOT_RESUME = 1008,
        ERROR_FILE_ALREADY_EXISTS = 1009;
    public static final int PAUSED_WAITING_TO_RETRY = 1, PAUSED_WAITING_FOR_NETWORK = 2, PAUSED_QUEUED_FOR_WIFI = 3, PAUSED_UNKNOWN = 4;
    public static final String ACTION_DOWNLOAD_COMPLETE = "android.intent.action.DOWNLOAD_COMPLETE",
        ACTION_NOTIFICATION_CLICKED = "android.intent.action.DOWNLOAD_NOTIFICATION_CLICKED",
        ACTION_VIEW_DOWNLOADS = "android.intent.action.VIEW_DOWNLOADS", EXTRA_DOWNLOAD_ID = "extra_download_id",
        EXTRA_NOTIFICATION_CLICK_DOWNLOAD_IDS = "extra_click_download_ids",
        INTENT_EXTRAS_SORT_BY_SIZE = "android.app.DownloadManager.extra_sortBySize";
    private static final String[] COLUMNS = {COLUMN_ID, COLUMN_TITLE, COLUMN_DESCRIPTION, COLUMN_URI, COLUMN_MEDIA_TYPE,
        COLUMN_TOTAL_SIZE_BYTES, COLUMN_LOCAL_URI, COLUMN_STATUS, COLUMN_REASON, COLUMN_BYTES_DOWNLOADED_SO_FAR,
        COLUMN_LAST_MODIFIED_TIMESTAMP, COLUMN_MEDIAPROVIDER_URI};
    private static final LinkedHashMap<Long, Object[]> downloads = new LinkedHashMap<>();
    private static long nextId = 1;

    public static class Request {
        public static final int NETWORK_MOBILE = 1, NETWORK_WIFI = 2;
        public static final int VISIBILITY_VISIBLE = 0, VISIBILITY_VISIBLE_NOTIFY_COMPLETED = 1, VISIBILITY_HIDDEN = 2,
            VISIBILITY_VISIBLE_NOTIFY_ONLY_COMPLETION = 3;
        private final Uri uri;
        private CharSequence title, description;
        private String mimeType;

        public Request(Uri uri) {
            if (uri == null) throw new NullPointerException();
            String scheme = uri.getScheme();
            if (scheme == null || !(scheme.equals("http") || scheme.equals("https")))
                throw new IllegalArgumentException("Can only download HTTP/HTTPS URIs: " + uri);
            this.uri = uri;
        }
        public Request setTitle(CharSequence title) { this.title = title; return this; }
        public Request setDescription(CharSequence description) { this.description = description; return this; }
        public Request setMimeType(String mimeType) { this.mimeType = mimeType; return this; }
        public Request addRequestHeader(String header, String value) { return this; }
        public Request setDestinationUri(Uri uri) { return this; }
        public Request setDestinationInExternalFilesDir(Context context, String dirType, String subPath) { return this; }
        public Request setDestinationInExternalPublicDir(String dirType, String subPath) { return this; }
        public Request setNotificationVisibility(int visibility) { return this; }
        @Deprecated public Request setShowRunningNotification(boolean show) { return this; }
        public Request setAllowedNetworkTypes(int flags) { return this; }
        public Request setAllowedOverMetered(boolean allow) { return this; }
        public Request setAllowedOverRoaming(boolean allowed) { return this; }
        public Request setRequiresCharging(boolean requiresCharging) { return this; }
        public Request setRequiresDeviceIdle(boolean requiresDeviceIdle) { return this; }
        @Deprecated public Request setVisibleInDownloadsUi(boolean isVisible) { return this; }
        @Deprecated public void allowScanningByMediaScanner() {}
    }

    public static class Query {
        private long[] ids;
        private int statusFlags;

        public Query() {}
        public Query setFilterById(long... ids) { this.ids = ids; return this; }
        public Query setFilterByStatus(int flags) { statusFlags = flags; return this; }
        boolean matches(Object[] row) {
            if (statusFlags != 0 && ((Integer) row[7] & statusFlags) == 0) return false;
            if (ids == null) return true;
            for (long id : ids) if (id == (Long) row[0]) return true;
            return false;
        }
    }

    public DownloadManager() {}
    public long enqueue(Request request) {
        long id;
        synchronized (downloads) {
            id = nextId++;
            downloads.put(id, new Object[] {id, request.title != null ? request.title.toString() : null,
                request.description != null ? request.description.toString() : null, request.uri.toString(), request.mimeType,
                -1L, null, STATUS_FAILED, ERROR_UNKNOWN, 0L, System.currentTimeMillis(), null});
        }
        Context app = snx.android.app.App.application();
        if (app != null) {
            Intent done = new Intent(ACTION_DOWNLOAD_COMPLETE).setPackage(app.getPackageName()).putExtra(EXTRA_DOWNLOAD_ID, id);
            new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> app.sendBroadcast(done));
        }
        return id;
    }
    public int remove(long... ids) {
        int n = 0;
        synchronized (downloads) { for (long id : ids) if (downloads.remove(id) != null) ++n; }
        return n;
    }
    public Cursor query(Query query) {
        MatrixCursor c = new MatrixCursor(COLUMNS);
        synchronized (downloads) { for (Object[] row : downloads.values()) if (query.matches(row)) c.addRow(row); }
        return c;
    }
    public Uri getUriForDownloadedFile(long id) { return null; }
    public String getMimeTypeForDownloadedFile(long id) { return null; }
    public android.os.ParcelFileDescriptor openDownloadedFile(long id) throws java.io.FileNotFoundException {
        throw new java.io.FileNotFoundException("No file found for download " + id);
    }
    public static Long getMaxBytesOverMobile(Context context) { return null; }
    public static Long getRecommendedMaxBytesOverMobile(Context context) { return null; }
}
