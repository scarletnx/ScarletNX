// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.http;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.net.CacheRequest;
import java.net.CacheResponse;
import java.net.ResponseCache;
import java.net.URI;
import java.net.URLConnection;
import java.util.List;
import java.util.Map;

/**
 * The HTTP cache of URLConnection. It is installed as the default ResponseCache and counts requests, but it keeps
 * no responses: each request goes to the network.
 */
public final class HttpResponseCache extends ResponseCache implements Closeable {
    private final File directory;
    private final long maxSize;
    private int requestCount, networkCount;

    private HttpResponseCache(File directory, long maxSize) { this.directory = directory; this.maxSize = maxSize; }

    public static HttpResponseCache getInstalled() {
        return ResponseCache.getDefault() instanceof HttpResponseCache c ? c : null;
    }

    public static synchronized HttpResponseCache install(File directory, long maxSize) throws IOException {
        HttpResponseCache installed = getInstalled();
        if (installed != null && installed.directory.equals(directory) && installed.maxSize == maxSize) return installed;
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("cannot create " + directory);
        HttpResponseCache cache = new HttpResponseCache(directory, maxSize);
        ResponseCache.setDefault(cache);
        return cache;
    }

    @Override public synchronized CacheResponse get(URI uri, String requestMethod, Map<String, List<String>> requestHeaders) {
        ++requestCount;
        ++networkCount;
        return null;
    }

    @Override public CacheRequest put(URI uri, URLConnection urlConnection) { return null; }

    public long size() { return 0; }
    public long maxSize() { return maxSize; }
    public void flush() {}
    public synchronized int getNetworkCount() { return networkCount; }
    public int getHitCount() { return 0; }
    public synchronized int getRequestCount() { return requestCount; }

    @Override public void close() throws IOException {
        if (ResponseCache.getDefault() == this) ResponseCache.setDefault(null);
    }

    public void delete() throws IOException { close(); }
}
