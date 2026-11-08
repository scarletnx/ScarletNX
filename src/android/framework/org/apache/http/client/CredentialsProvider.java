// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client;

import org.apache.http.auth.AuthScope;
import org.apache.http.auth.Credentials;

public interface CredentialsProvider {
    void setCredentials(AuthScope authscope, Credentials credentials);
    Credentials getCredentials(AuthScope authscope);
    void clear();
}
