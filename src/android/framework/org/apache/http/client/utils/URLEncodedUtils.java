// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client.utils;

import java.net.URLDecoder;
import java.net.URLEncoder;
import java.util.ArrayList;
import java.util.List;
import org.apache.http.NameValuePair;
import org.apache.http.message.BasicNameValuePair;

public class URLEncodedUtils {
    public static final String CONTENT_TYPE = "application/x-www-form-urlencoded";

    public URLEncodedUtils() {}
    public static String format(List<? extends NameValuePair> parameters, String encoding) {
        String charset = encoding != null ? encoding : "ISO-8859-1";
        StringBuilder result = new StringBuilder();
        try {
            for (NameValuePair p : parameters) {
                if (result.length() > 0) result.append('&');
                result.append(URLEncoder.encode(p.getName(), charset));
                if (p.getValue() != null) result.append('=').append(URLEncoder.encode(p.getValue(), charset));
            }
        } catch (java.io.UnsupportedEncodingException e) {
            throw new IllegalArgumentException(e);
        }
        return result.toString();
    }
    public static List<NameValuePair> parse(java.net.URI uri, String encoding) {
        List<NameValuePair> result = new ArrayList<>();
        String query = uri.getRawQuery();
        if (query == null || query.isEmpty()) return result;
        String charset = encoding != null ? encoding : "ISO-8859-1";
        try {
            for (String part : query.split("&")) {
                int eq = part.indexOf('=');
                String name = URLDecoder.decode(eq >= 0 ? part.substring(0, eq) : part, charset);
                String value = eq >= 0 ? URLDecoder.decode(part.substring(eq + 1), charset) : null;
                result.add(new BasicNameValuePair(name, value));
            }
        } catch (java.io.UnsupportedEncodingException e) {
            throw new IllegalArgumentException(e);
        }
        return result;
    }
}
