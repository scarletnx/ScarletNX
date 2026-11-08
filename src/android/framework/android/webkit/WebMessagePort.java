// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** A port of a page's message channel: no page holds the other end */
public abstract class WebMessagePort {
    public abstract static class WebMessageCallback {
        public void onMessage(WebMessagePort port, WebMessage message) {}
    }
    public abstract void postMessage(WebMessage message);
    public abstract void close();
    public abstract void setWebMessageCallback(WebMessageCallback callback);
    public abstract void setWebMessageCallback(WebMessageCallback callback, android.os.Handler handler);
}
