// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.protocol;

public final class HTTP {
    public static final String UTF_8 = "UTF-8", UTF_16 = "UTF-16", US_ASCII = "US-ASCII", ASCII = "ASCII",
        ISO_8859_1 = "ISO-8859-1", DEFAULT_CONTENT_CHARSET = ISO_8859_1, DEFAULT_PROTOCOL_CHARSET = US_ASCII,
        OCTET_STREAM_TYPE = "application/octet-stream", PLAIN_TEXT_TYPE = "text/plain", CHARSET_PARAM = "; charset=",
        DEFAULT_CONTENT_TYPE = OCTET_STREAM_TYPE, TRANSFER_ENCODING = "Transfer-Encoding",
        CONTENT_LEN = "Content-Length", CONTENT_TYPE = "Content-Type", CONTENT_ENCODING = "Content-Encoding",
        EXPECT_DIRECTIVE = "Expect", CONN_DIRECTIVE = "Connection", TARGET_HOST = "Host", USER_AGENT = "User-Agent",
        DATE_HEADER = "Date", SERVER_HEADER = "Server", CHUNK_CODING = "chunked", IDENTITY_CODING = "identity",
        CONN_CLOSE = "Close", CONN_KEEP_ALIVE = "Keep-Alive", EXPECT_CONTINUE = "100-continue";
    private HTTP() {}
}
