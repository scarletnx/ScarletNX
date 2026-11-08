// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client.entity;

import java.io.UnsupportedEncodingException;
import java.util.List;
import org.apache.http.NameValuePair;
import org.apache.http.client.utils.URLEncodedUtils;
import org.apache.http.entity.StringEntity;

public class UrlEncodedFormEntity extends StringEntity {
    public UrlEncodedFormEntity(List<? extends NameValuePair> parameters, String encoding) throws UnsupportedEncodingException {
        super(URLEncodedUtils.format(parameters, encoding), encoding);
        setContentType("application/x-www-form-urlencoded" + (encoding != null ? "; charset=" + encoding : ""));
    }
    public UrlEncodedFormEntity(List<? extends NameValuePair> parameters) throws UnsupportedEncodingException {
        this(parameters, "ISO-8859-1");
    }
}
