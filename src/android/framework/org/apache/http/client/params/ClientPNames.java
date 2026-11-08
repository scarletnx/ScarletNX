// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client.params;

public interface ClientPNames {
    String CONNECTION_MANAGER_FACTORY_CLASS_NAME = "http.connection-manager.factory-class-name",
        CONNECTION_MANAGER_FACTORY = "http.connection-manager.factory-object",
        HANDLE_REDIRECTS = "http.protocol.handle-redirects", REJECT_RELATIVE_REDIRECT = "http.protocol.reject-relative-redirect",
        MAX_REDIRECTS = "http.protocol.max-redirects", ALLOW_CIRCULAR_REDIRECTS = "http.protocol.allow-circular-redirects",
        HANDLE_AUTHENTICATION = "http.protocol.handle-authentication", COOKIE_POLICY = "http.protocol.cookie-policy",
        VIRTUAL_HOST = "http.virtual-host", DEFAULT_HEADERS = "http.default-headers", DEFAULT_HOST = "http.default-host";
}
