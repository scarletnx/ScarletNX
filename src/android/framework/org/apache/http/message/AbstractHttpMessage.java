// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.message;

import java.util.ArrayList;
import java.util.Iterator;
import org.apache.http.Header;
import org.apache.http.HeaderIterator;
import org.apache.http.HttpMessage;
import org.apache.http.params.BasicHttpParams;
import org.apache.http.params.HttpParams;

/** A message's headers, in order, and its parameters. */
public abstract class AbstractHttpMessage implements HttpMessage {
    protected final ArrayList<Header> headergroup = new ArrayList<>();
    protected HttpParams params;

    protected AbstractHttpMessage(HttpParams params) { this.params = params; }
    protected AbstractHttpMessage() { this(null); }

    @Override public boolean containsHeader(String name) { return getFirstHeader(name) != null; }
    @Override public Header[] getHeaders(String name) {
        ArrayList<Header> out = new ArrayList<>();
        for (Header h : headergroup)
            if (h.getName().equalsIgnoreCase(name)) out.add(h);
        return out.toArray(new Header[0]);
    }
    @Override public Header getFirstHeader(String name) {
        for (Header h : headergroup)
            if (h.getName().equalsIgnoreCase(name)) return h;
        return null;
    }
    @Override public Header getLastHeader(String name) {
        for (int i = headergroup.size() - 1; i >= 0; --i)
            if (headergroup.get(i).getName().equalsIgnoreCase(name)) return headergroup.get(i);
        return null;
    }
    @Override public Header[] getAllHeaders() { return headergroup.toArray(new Header[0]); }
    @Override public void addHeader(Header header) { if (header != null) headergroup.add(header); }
    @Override public void addHeader(String name, String value) { addHeader(new BasicHeader(name, value)); }
    @Override public void setHeader(Header header) {
        if (header == null) return;
        for (int i = 0; i < headergroup.size(); ++i)
            if (headergroup.get(i).getName().equalsIgnoreCase(header.getName())) {
                headergroup.set(i, header);
                return;
            }
        headergroup.add(header);
    }
    @Override public void setHeader(String name, String value) { setHeader(new BasicHeader(name, value)); }
    @Override public void setHeaders(Header[] headers) {
        headergroup.clear();
        if (headers != null)
            for (Header h : headers) headergroup.add(h);
    }
    @Override public void removeHeader(Header header) { headergroup.remove(header); }
    @Override public void removeHeaders(String name) { headergroup.removeIf(h -> h.getName().equalsIgnoreCase(name)); }
    @Override public HeaderIterator headerIterator() { return iterator(getAllHeaders()); }
    @Override public HeaderIterator headerIterator(String name) { return iterator(getHeaders(name)); }
    private static HeaderIterator iterator(Header[] headers) {
        Iterator<Header> it = java.util.Arrays.asList(headers).iterator();
        return new HeaderIterator() {
            @Override public boolean hasNext() { return it.hasNext(); }
            @Override public Header nextHeader() { return it.next(); }
            @Override public Object next() { return it.next(); }
        };
    }
    @Override public HttpParams getParams() {
        if (params == null) params = new BasicHttpParams();
        return params;
    }
    @Override public void setParams(HttpParams params) {
        if (params == null) throw new IllegalArgumentException("HTTP parameters may not be null");
        this.params = params;
    }
}
