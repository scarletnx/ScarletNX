// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

import android.graphics.Bitmap;
import android.view.KeyEvent;

/** A WebView's client: the callbacks of a page's loading, each doing nothing as Android's base class */
public class WebViewClient {
    public static final int ERROR_UNKNOWN = -1, ERROR_HOST_LOOKUP = -2, ERROR_CONNECT = -6, ERROR_TIMEOUT = -8;
    public WebViewClient() {}
    @Deprecated public boolean shouldOverrideUrlLoading(WebView view, String url) { return false; }
    public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) { return shouldOverrideUrlLoading(view, request.getUrl().toString()); }
    public void onPageStarted(WebView view, String url, Bitmap favicon) {}
    public void onPageFinished(WebView view, String url) {}
    public void onLoadResource(WebView view, String url) {}
    public void onPageCommitVisible(WebView view, String url) {}
    @Deprecated public WebResourceResponse shouldInterceptRequest(WebView view, String url) { return null; }
    public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) { return shouldInterceptRequest(view, request.getUrl().toString()); }
    @Deprecated public void onReceivedError(WebView view, int errorCode, String description, String failingUrl) {}
    public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {}
    public void onReceivedHttpError(WebView view, WebResourceRequest request, WebResourceResponse errorResponse) {}
    public void onReceivedSslError(WebView view, SslErrorHandler handler, android.net.http.SslError error) { handler.cancel(); }
    public void onUnhandledKeyEvent(WebView view, KeyEvent event) {}
    public boolean shouldOverrideKeyEvent(WebView view, KeyEvent event) { return false; }
    public void doUpdateVisitedHistory(WebView view, String url, boolean isReload) {}
    public void onScaleChanged(WebView view, float oldScale, float newScale) {}
    public boolean onRenderProcessGone(WebView view, RenderProcessGoneDetail detail) { return false; }
    public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler, String host, String realm) { handler.cancel(); }
}
