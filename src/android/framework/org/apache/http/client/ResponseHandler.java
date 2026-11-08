// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client;

import org.apache.http.HttpResponse;

public interface ResponseHandler<T> {
    T handleResponse(HttpResponse response) throws ClientProtocolException, java.io.IOException;
}
