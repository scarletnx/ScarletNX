// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

import android.content.Context;
import android.graphics.Canvas;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import android.widget.FrameLayout;

/**
 * A web page's view (Off in docs/platforms/android-apps/interfaces.md: no web engine). It keeps its settings and clients, and a page
 * it loads starts and finishes empty, as a blank page, so an app's flow goes on: onPageStarted and onPageFinished
 * come, a script's result is "null", and nothing draws.
 */
public class WebView extends FrameLayout {
    public static final int RENDERER_PRIORITY_WAIVED = 0, RENDERER_PRIORITY_BOUND = 1, RENDERER_PRIORITY_IMPORTANT = 2;

    public class WebViewTransport {
        private WebView view;
        public synchronized void setWebView(WebView webview) { view = webview; }
        public synchronized WebView getWebView() { return view; }
    }
    public static class HitTestResult {
        public static final int UNKNOWN_TYPE = 0;
        public int getType() { return UNKNOWN_TYPE; }
        public String getExtra() { return null; }
    }

    private final WebSettings settings = new WebSettings.Values();
    private WebViewClient client = new WebViewClient();
    private WebChromeClient chrome;
    private String url, title;
    private final Handler main = new Handler(Looper.getMainLooper());

    public WebView(Context context) { this(context, null); }
    public WebView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public WebView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public WebView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    public static void setWebContentsDebuggingEnabled(boolean enabled) {}
    public static void setDataDirectorySuffix(String suffix) {}
    public static String findAddress(String addr) { return null; }
    public static PackageInfoHolder getCurrentWebViewPackage() { return null; }
    /** WebView's package: none */
    public static final class PackageInfoHolder {}
    public static void clearClientCertPreferences(Runnable onCleared) { if (onCleared != null) onCleared.run(); }

    public WebSettings getSettings() { return settings; }
    public void setWebViewClient(WebViewClient client) { this.client = client != null ? client : new WebViewClient(); }
    public WebViewClient getWebViewClient() { return client; }
    public void setWebChromeClient(WebChromeClient client) { chrome = client; }
    public WebChromeClient getWebChromeClient() { return chrome; }

    /** a page that loads blank: the client hears it start and finish */
    public void loadUrl(String url) {
        this.url = url;
        main.post(() -> {
            client.onPageStarted(this, url, null);
            if (chrome != null) chrome.onProgressChanged(this, 100);
            client.onPageFinished(this, url);
        });
    }
    public void loadUrl(String url, java.util.Map<String, String> additionalHttpHeaders) { loadUrl(url); }
    public void loadData(String data, String mimeType, String encoding) { loadUrl("about:blank"); }
    public void loadDataWithBaseURL(String baseUrl, String data, String mimeType, String encoding, String historyUrl) {
        loadUrl(baseUrl != null ? baseUrl : "about:blank");
    }
    public void postUrl(String url, byte[] postData) { loadUrl(url); }
    public void reload() { if (url != null) loadUrl(url); }
    public void stopLoading() {}
    public boolean canGoBack() { return false; }
    public boolean canGoForward() { return false; }
    public void goBack() {}
    public void goForward() {}
    public String getUrl() { return url; }
    public String getOriginalUrl() { return url; }
    public String getTitle() { return title; }
    public int getProgress() { return 100; }
    public int getContentHeight() { return 0; }
    public void evaluateJavascript(String script, ValueCallback<String> resultCallback) {
        if (resultCallback != null) main.post(() -> resultCallback.onReceiveValue("null"));
    }
    public void addJavascriptInterface(Object object, String name) {}
    public void removeJavascriptInterface(String name) {}
    public void clearCache(boolean includeDiskFiles) {}
    public void clearHistory() {}
    public void clearFormData() {}
    public void clearSslPreferences() {}
    public void onPause() {}
    public void onResume() {}
    public void pauseTimers() {}
    public void resumeTimers() {}
    public void destroy() {}
    public void setInitialScale(int scaleInPercent) {}
    public void setNetworkAvailable(boolean networkUp) {}
    public void setDownloadListener(Object listener) {}
    public void setFindListener(Object listener) {}
    public void setRendererPriorityPolicy(int rendererRequestedPriority, boolean waivedWhenNotVisible) {}
    public HitTestResult getHitTestResult() { return new HitTestResult(); }
    public void requestFocusNodeHref(android.os.Message hrefMsg) {}
    public boolean zoomIn() { return false; }
    public boolean zoomOut() { return false; }
    public void flingScroll(int vx, int vy) {}
    public boolean pageUp(boolean top) { return false; }
    public boolean pageDown(boolean bottom) { return false; }
    public static void startSafeBrowsing(Context context, ValueCallback<Boolean> callback) {
        if (callback != null) callback.onReceiveValue(false);
    }
    public void buildLayer() {}
    public void setScrollContainer(boolean isScrollContainer) {}
    protected void onOverScrolled(int scrollX, int scrollY, boolean clampedX, boolean clampedY) { scrollTo(scrollX, scrollY); }
    @Override protected void onDraw(Canvas canvas) {}
}
