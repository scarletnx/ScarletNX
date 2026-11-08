// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.cookie;

import java.util.Date;

public interface SetCookie extends Cookie {
    void setValue(String value);
    void setComment(String comment);
    void setExpiryDate(Date expiryDate);
    void setDomain(String domain);
    void setPath(String path);
    void setSecure(boolean secure);
    void setVersion(int version);
}
