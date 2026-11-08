// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** URL tests and fixes, as Android's (their prefixes, as Android checks them) */
public final class URLUtil {
    private URLUtil() {}

    private static boolean starts(String url, String prefix) {
        return url != null && url.length() >= prefix.length() && url.regionMatches(true, 0, prefix, 0, prefix.length());
    }
    public static boolean isAssetUrl(String url) { return url != null && url.startsWith("file:///android_asset/"); }
    public static boolean isResourceUrl(String url) { return url != null && url.startsWith("file:///android_res/"); }
    @Deprecated public static boolean isCookielessProxyUrl(String url) { return url != null && url.startsWith("file:///cookieless_proxy/"); }
    public static boolean isFileUrl(String url) {
        return url != null && url.startsWith("file://") && !url.startsWith("file:///android_asset/") && !url.startsWith("file:///cookieless_proxy/");
    }
    public static boolean isAboutUrl(String url) { return starts(url, "about:"); }
    public static boolean isDataUrl(String url) { return starts(url, "data:"); }
    public static boolean isJavaScriptUrl(String url) { return starts(url, "javascript:"); }
    public static boolean isHttpUrl(String url) { return url != null && url.length() > 6 && url.substring(0, 7).equalsIgnoreCase("http://"); }
    public static boolean isHttpsUrl(String url) { return url != null && url.length() > 7 && url.substring(0, 8).equalsIgnoreCase("https://"); }
    public static boolean isNetworkUrl(String url) { return url != null && !url.isEmpty() && (isHttpUrl(url) || isHttpsUrl(url)); }
    public static boolean isContentUrl(String url) { return starts(url, "content:"); }
    public static boolean isValidUrl(String url) {
        return url != null && !url.isEmpty() && (isAssetUrl(url) || isResourceUrl(url) || isFileUrl(url) || isAboutUrl(url)
            || isHttpUrl(url) || isHttpsUrl(url) || isJavaScriptUrl(url) || isContentUrl(url));
    }
    /** a URL without its fragment */
    public static String stripAnchor(String url) {
        int anchor = url.indexOf('#');
        return anchor != -1 ? url.substring(0, anchor) : url;
    }
    /** a user's text as a URL: a scheme added when it has none (http) */
    public static String guessUrl(String inUrl) {
        String url = inUrl.trim();
        if (url.isEmpty() || url.startsWith("about:") || url.startsWith("data:") || url.startsWith("file:") || url.startsWith("javascript:")) return url;
        if (url.endsWith(".")) url = url.substring(0, url.length() - 1);
        try {
            java.net.URI u = new java.net.URI(url);
            if (u.getScheme() == null) url = "http://" + url;
        } catch (java.net.URISyntaxException e) {
            url = "http://" + url;
        }
        return url;
    }
    /** a download's file name: the Content-Disposition's, else the URL's last part, with an extension of its type */
    public static String guessFileName(String url, String contentDisposition, String mimeType) {
        String name = null;
        if (contentDisposition != null) {
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("attachment;\\s*filename\\s*=\\s*(\"?)([^\"]*)\\1\\s*$",
                java.util.regex.Pattern.CASE_INSENSITIVE).matcher(contentDisposition);
            if (m.find()) name = m.group(2);
            if (name != null) name = name.substring(name.lastIndexOf('/') + 1);
        }
        if (name == null) {
            String decoded = android.net.Uri.decode(url);
            if (decoded != null) {
                int q = decoded.indexOf('?');
                if (q > 0) decoded = decoded.substring(0, q);
                if (!decoded.endsWith("/")) {
                    int slash = decoded.lastIndexOf('/') + 1;
                    if (slash > 0) name = decoded.substring(slash);
                }
            }
        }
        if (name == null || name.isEmpty()) name = "downloadfile";
        if (name.indexOf('.') < 0) {
            String ext = mimeType != null ? MimeTypeMap.getSingleton().getExtensionFromMimeType(mimeType) : null;
            name += ext != null ? "." + ext : mimeType != null && mimeType.toLowerCase().startsWith("text/")
                ? (mimeType.equalsIgnoreCase("text/html") ? ".html" : ".txt") : ".bin";
        }
        return name;
    }
}
