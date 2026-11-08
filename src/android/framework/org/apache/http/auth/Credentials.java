// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.auth;

public interface Credentials {
    java.security.Principal getUserPrincipal();
    String getPassword();
}
