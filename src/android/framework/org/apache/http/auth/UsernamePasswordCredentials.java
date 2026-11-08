// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.auth;

import java.security.Principal;

public class UsernamePasswordCredentials implements Credentials {
    private final String userName, password;

    public UsernamePasswordCredentials(String userName, String password) {
        if (userName == null) throw new IllegalArgumentException("Username may not be null");
        this.userName = userName;
        this.password = password;
    }
    public UsernamePasswordCredentials(String usernamePassword) {
        int colon = usernamePassword.indexOf(':');
        userName = colon >= 0 ? usernamePassword.substring(0, colon) : usernamePassword;
        password = colon >= 0 ? usernamePassword.substring(colon + 1) : null;
    }
    public String getUserName() { return userName; }
    @Override public Principal getUserPrincipal() { return () -> userName; }
    @Override public String getPassword() { return password; }
}
