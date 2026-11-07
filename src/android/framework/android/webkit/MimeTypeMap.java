// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

import java.util.Map;

/** MIME types by file extension. */
public class MimeTypeMap {
    private static final MimeTypeMap INSTANCE = new MimeTypeMap();
    private static final Map<String, String> TYPES = Map.ofEntries(Map.entry("txt", "text/plain"), Map.entry("html", "text/html"),
        Map.entry("htm", "text/html"), Map.entry("css", "text/css"), Map.entry("js", "text/javascript"), Map.entry("json", "application/json"),
        Map.entry("xml", "text/xml"), Map.entry("csv", "text/csv"), Map.entry("md", "text/markdown"), Map.entry("png", "image/png"),
        Map.entry("jpg", "image/jpeg"), Map.entry("jpeg", "image/jpeg"), Map.entry("gif", "image/gif"), Map.entry("webp", "image/webp"),
        Map.entry("svg", "image/svg+xml"), Map.entry("pdf", "application/pdf"), Map.entry("zip", "application/zip"),
        Map.entry("mp3", "audio/mpeg"), Map.entry("mp4", "video/mp4"), Map.entry("apk", "application/vnd.android.package-archive"));

    public static MimeTypeMap getSingleton() { return INSTANCE; }
    public static String getFileExtensionFromUrl(String url) {
        if (url == null) return "";
        int q = url.indexOf('?');
        if (q >= 0) url = url.substring(0, q);
        int slash = url.lastIndexOf('/'), dot = url.lastIndexOf('.');
        return dot > slash ? url.substring(dot + 1) : "";
    }
    public boolean hasMimeType(String mimeType) { return TYPES.containsValue(mimeType); }
    public String getMimeTypeFromExtension(String extension) { return extension != null ? TYPES.get(extension.toLowerCase(java.util.Locale.ROOT)) : null; }
    public boolean hasExtension(String extension) { return getMimeTypeFromExtension(extension) != null; }
    public String getExtensionFromMimeType(String mimeType) {
        for (Map.Entry<String, String> e : TYPES.entrySet()) if (e.getValue().equals(mimeType)) return e.getKey();
        return null;
    }
}
