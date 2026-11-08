// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.util;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import org.apache.http.HttpEntity;

public final class EntityUtils {
    private EntityUtils() {}
    public static byte[] toByteArray(HttpEntity entity) throws IOException {
        if (entity == null) throw new IllegalArgumentException("HTTP entity may not be null");
        try (InputStream in = entity.getContent()) {
            if (in == null) return new byte[0];
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            in.transferTo(out);
            return out.toByteArray();
        }
    }
    public static String getContentCharSet(HttpEntity entity) {
        if (entity == null || entity.getContentType() == null) return null;
        for (org.apache.http.HeaderElement e : entity.getContentType().getElements()) {
            org.apache.http.NameValuePair p = e.getParameterByName("charset");
            if (p != null) return p.getValue();
        }
        return null;
    }
    public static String toString(HttpEntity entity, String defaultCharset) throws IOException {
        String charset = getContentCharSet(entity);
        return new String(toByteArray(entity), charset != null ? charset : defaultCharset != null ? defaultCharset : "ISO-8859-1");
    }
    public static String toString(HttpEntity entity) throws IOException { return toString(entity, null); }
}
