// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

public interface RequestLine {
    String getMethod();
    ProtocolVersion getProtocolVersion();
    String getUri();
}
