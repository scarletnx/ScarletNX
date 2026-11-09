// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** An entry of a WebView's history: the page's URL, its first URL and its title. */
public class WebHistoryItem implements Cloneable {
    private final String url, originalUrl, title;

    WebHistoryItem(String url, String originalUrl, String title) {
        this.url = url;
        this.originalUrl = originalUrl;
        this.title = title;
    }
    @Deprecated public int getId() { return System.identityHashCode(this); }
    public String getUrl() { return url; }
    public String getOriginalUrl() { return originalUrl; }
    public String getTitle() { return title; }
    public android.graphics.Bitmap getFavicon() { return null; }
    @Override protected WebHistoryItem clone() { return new WebHistoryItem(url, originalUrl, title); }
}
