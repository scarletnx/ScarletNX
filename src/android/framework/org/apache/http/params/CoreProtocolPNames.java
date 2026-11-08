// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.params;

public interface CoreProtocolPNames {
    String PROTOCOL_VERSION = "http.protocol.version", HTTP_ELEMENT_CHARSET = "http.protocol.element-charset",
        HTTP_CONTENT_CHARSET = "http.protocol.content-charset", USER_AGENT = "http.useragent",
        ORIGIN_SERVER = "http.origin-server", STRICT_TRANSFER_ENCODING = "http.protocol.strict-transfer-encoding",
        USE_EXPECT_CONTINUE = "http.protocol.expect-continue", WAIT_FOR_CONTINUE = "http.protocol.wait-for-continue";
}
