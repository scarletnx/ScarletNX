// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client;

import java.util.Date;
import java.util.List;
import org.apache.http.cookie.Cookie;

public interface CookieStore {
    void addCookie(Cookie cookie);
    List<Cookie> getCookies();
    boolean clearExpired(Date date);
    void clear();
}
