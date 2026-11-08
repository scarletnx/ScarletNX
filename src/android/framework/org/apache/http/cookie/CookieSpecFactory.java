// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.cookie;

import org.apache.http.params.HttpParams;

public interface CookieSpecFactory {
    CookieSpec newInstance(HttpParams params);
}
