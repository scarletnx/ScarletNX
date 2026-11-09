// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** A page's HTTP authentication request, which the app answers */
public class HttpAuthHandler extends android.os.Handler {
    HttpAuthHandler() {}
    public boolean useHttpAuthUsernamePassword() { return false; }
    public void cancel() {}
    public void proceed(String username, String password) {}
}
