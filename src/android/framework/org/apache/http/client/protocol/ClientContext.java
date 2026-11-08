// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client.protocol;

public interface ClientContext {
    String COOKIESPEC_REGISTRY = "http.cookiespec-registry", AUTHSCHEME_REGISTRY = "http.authscheme-registry",
        COOKIE_STORE = "http.cookie-store", COOKIE_SPEC = "http.cookie-spec", COOKIE_ORIGIN = "http.cookie-origin",
        CREDS_PROVIDER = "http.auth.credentials-provider", TARGET_AUTH_STATE = "http.auth.target-scope",
        PROXY_AUTH_STATE = "http.auth.proxy-scope", AUTH_SCHEME_PREF = "http.auth.scheme-pref",
        USER_TOKEN = "http.user-token";
}
