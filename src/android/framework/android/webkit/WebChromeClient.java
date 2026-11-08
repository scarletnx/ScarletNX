// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

import android.graphics.Bitmap;
import android.view.View;

/** A WebView's browser-side callbacks, each doing nothing as Android's base class */
public class WebChromeClient {
    public interface CustomViewCallback { void onCustomViewHidden(); }
    public abstract static class FileChooserParams {
        public static final int MODE_OPEN = 0, MODE_OPEN_MULTIPLE = 1, MODE_SAVE = 3;
        public abstract int getMode();
        public abstract String[] getAcceptTypes();
        public abstract boolean isCaptureEnabled();
        public abstract CharSequence getTitle();
        public abstract String getFilenameHint();
        public abstract android.content.Intent createIntent();
        public static android.net.Uri[] parseResult(int resultCode, android.content.Intent data) { return null; }
    }
    public WebChromeClient() {}
    public void onProgressChanged(WebView view, int newProgress) {}
    public void onReceivedTitle(WebView view, String title) {}
    public void onReceivedIcon(WebView view, Bitmap icon) {}
    public void onShowCustomView(View view, CustomViewCallback callback) {}
    public void onHideCustomView() {}
    public boolean onCreateWindow(WebView view, boolean isDialog, boolean isUserGesture, android.os.Message resultMsg) { return false; }
    public void onCloseWindow(WebView window) {}
    public boolean onJsAlert(WebView view, String url, String message, JsResult result) { return false; }
    public boolean onJsConfirm(WebView view, String url, String message, JsResult result) { return false; }
    public void onPermissionRequest(PermissionRequest request) { request.deny(); }
    public void onPermissionRequestCanceled(PermissionRequest request) {}
    public boolean onConsoleMessage(ConsoleMessage consoleMessage) { return false; }
    public Bitmap getDefaultVideoPoster() { return null; }
    public View getVideoLoadingProgressView() { return null; }
    public boolean onShowFileChooser(WebView webView, ValueCallback<android.net.Uri[]> filePathCallback, FileChooserParams fileChooserParams) { return false; }
}
