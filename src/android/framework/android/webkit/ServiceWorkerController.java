// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** The service workers of the pages: their client and settings are kept */
public abstract class ServiceWorkerController {
    private static final ServiceWorkerController INSTANCE = new ServiceWorkerController() {
        private final ServiceWorkerWebSettings settings = new ServiceWorkerWebSettings.Values();
        private ServiceWorkerClient client;
        @Override public ServiceWorkerWebSettings getServiceWorkerWebSettings() { return settings; }
        @Override public void setServiceWorkerClient(ServiceWorkerClient client) { this.client = client; }
    };
    public static ServiceWorkerController getInstance() { return INSTANCE; }
    public abstract ServiceWorkerWebSettings getServiceWorkerWebSettings();
    public abstract void setServiceWorkerClient(ServiceWorkerClient client);
}
