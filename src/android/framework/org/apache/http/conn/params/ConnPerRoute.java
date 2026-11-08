// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.params;

public interface ConnPerRoute {
    int getMaxForRoute(Object route);
}
