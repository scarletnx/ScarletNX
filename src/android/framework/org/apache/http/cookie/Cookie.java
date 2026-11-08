// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.cookie;

import java.util.Date;

public interface Cookie {
    String getName();
    String getValue();
    String getComment();
    String getCommentURL();
    Date getExpiryDate();
    boolean isPersistent();
    String getDomain();
    String getPath();
    int[] getPorts();
    boolean isSecure();
    int getVersion();
    boolean isExpired(Date date);
}
