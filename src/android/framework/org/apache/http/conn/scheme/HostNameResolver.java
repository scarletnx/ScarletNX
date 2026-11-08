// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.scheme;

public interface HostNameResolver {
    java.net.InetAddress resolve(String hostname) throws java.io.IOException;
}
