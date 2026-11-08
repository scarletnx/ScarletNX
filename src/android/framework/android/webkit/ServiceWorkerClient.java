// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** The service workers' requests, which the app may answer: none come, as no page runs */
public class ServiceWorkerClient {
    public ServiceWorkerClient() {}
    public WebResourceResponse shouldInterceptRequest(WebResourceRequest request) { return null; }
}
