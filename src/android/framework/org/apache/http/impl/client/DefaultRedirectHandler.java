// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.client;

import java.net.URI;
import org.apache.http.Header;
import org.apache.http.HttpResponse;
import org.apache.http.ProtocolException;
import org.apache.http.client.RedirectHandler;
import org.apache.http.protocol.HttpContext;

public class DefaultRedirectHandler implements RedirectHandler {
    public DefaultRedirectHandler() {}
    @Override public boolean isRedirectRequested(HttpResponse response, HttpContext context) {
        if (response == null) throw new IllegalArgumentException("HTTP response may not be null");
        int code = response.getStatusLine().getStatusCode();
        return code == 301 || code == 302 || code == 303 || code == 307;
    }
    @Override public URI getLocationURI(HttpResponse response, HttpContext context) throws ProtocolException {
        Header location = response.getFirstHeader("location");
        if (location == null)
            throw new ProtocolException("Received redirect response " + response.getStatusLine() + " but no location header");
        try {
            URI uri = new URI(location.getValue());
            Object request = context != null ? context.getAttribute("http.request") : null;
            if (!uri.isAbsolute() && request instanceof org.apache.http.client.methods.HttpUriRequest r && r.getURI() != null)
                uri = r.getURI().resolve(uri);
            return uri;
        } catch (java.net.URISyntaxException e) {
            throw new ProtocolException("Invalid redirect URI: " + location.getValue(), e);
        }
    }
}
