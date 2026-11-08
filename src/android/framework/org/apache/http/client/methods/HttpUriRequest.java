// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client.methods;

import java.net.URI;
import org.apache.http.HttpRequest;

public interface HttpUriRequest extends HttpRequest {
    String getMethod();
    URI getURI();
    void abort() throws UnsupportedOperationException;
    boolean isAborted();
}
