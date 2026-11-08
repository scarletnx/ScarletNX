// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client;

import java.net.URI;
import org.apache.http.HttpResponse;
import org.apache.http.ProtocolException;
import org.apache.http.protocol.HttpContext;

public interface RedirectHandler {
    boolean isRedirectRequested(HttpResponse response, HttpContext context);
    URI getLocationURI(HttpResponse response, HttpContext context) throws ProtocolException;
}
