// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.client;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Map;
import javax.net.ssl.HttpsURLConnection;
import org.apache.http.Header;
import org.apache.http.HttpEntity;
import org.apache.http.HttpEntityEnclosingRequest;
import org.apache.http.HttpException;
import org.apache.http.HttpHost;
import org.apache.http.HttpRequest;
import org.apache.http.HttpRequestInterceptor;
import org.apache.http.HttpResponse;
import org.apache.http.HttpResponseInterceptor;
import org.apache.http.HttpVersion;
import org.apache.http.client.ClientProtocolException;
import org.apache.http.client.CookieStore;
import org.apache.http.client.CredentialsProvider;
import org.apache.http.client.HttpClient;
import org.apache.http.client.HttpRequestRetryHandler;
import org.apache.http.client.RedirectHandler;
import org.apache.http.client.ResponseHandler;
import org.apache.http.client.methods.HttpUriRequest;
import org.apache.http.conn.ClientConnectionManager;
import org.apache.http.conn.EofSensorInputStream;
import org.apache.http.conn.scheme.SchemeRegistry;
import org.apache.http.cookie.Cookie;
import org.apache.http.cookie.CookieOrigin;
import org.apache.http.cookie.CookieSpec;
import org.apache.http.cookie.CookieSpecRegistry;
import org.apache.http.entity.BasicHttpEntity;
import org.apache.http.message.BasicHeader;
import org.apache.http.message.BasicHttpResponse;
import org.apache.http.params.HttpConnectionParams;
import org.apache.http.params.HttpParams;
import org.apache.http.params.HttpProtocolParams;
import org.apache.http.protocol.BasicHttpContext;
import org.apache.http.protocol.HttpContext;

/**
 * Android's legacy HTTP client over the JDK's HttpURLConnection, which keeps the connections, follows the redirects
 * that the redirect handler asks for, and speaks TLS. The client runs its interceptors, its retry handler and its
 * cookie store around each request, as Apache's does.
 */
public abstract class AbstractHttpClient implements HttpClient {
    private HttpParams defaultParams;
    private ClientConnectionManager connManager;
    private final ArrayList<HttpRequestInterceptor> requestInterceptors = new ArrayList<>();
    private final ArrayList<HttpResponseInterceptor> responseInterceptors = new ArrayList<>();
    private HttpRequestRetryHandler retryHandler;
    private RedirectHandler redirectHandler;
    private CredentialsProvider credsProvider;
    private CookieStore cookieStore;
    private CookieSpecRegistry supportedCookieSpecs;

    protected AbstractHttpClient(ClientConnectionManager conman, HttpParams params) {
        defaultParams = params;
        connManager = conman;
    }
    protected abstract HttpParams createHttpParams();
    protected abstract ClientConnectionManager createClientConnectionManager();

    @Override public final synchronized HttpParams getParams() {
        if (defaultParams == null) defaultParams = createHttpParams();
        return defaultParams;
    }
    public synchronized void setParams(HttpParams params) { defaultParams = params; }
    @Override public final synchronized ClientConnectionManager getConnectionManager() {
        if (connManager == null) connManager = createClientConnectionManager();
        return connManager;
    }
    public final synchronized CookieSpecRegistry getCookieSpecs() {
        if (supportedCookieSpecs == null) {
            supportedCookieSpecs = new CookieSpecRegistry();
            supportedCookieSpecs.register("compatibility", p -> new org.apache.http.impl.cookie.BrowserCompatSpec());
            supportedCookieSpecs.register("best-match", p -> new org.apache.http.impl.cookie.BrowserCompatSpec());
        }
        return supportedCookieSpecs;
    }
    public synchronized void setCookieSpecs(CookieSpecRegistry cookieSpecRegistry) { supportedCookieSpecs = cookieSpecRegistry; }
    public final synchronized CookieStore getCookieStore() {
        if (cookieStore == null) cookieStore = new BasicCookieStore();
        return cookieStore;
    }
    public synchronized void setCookieStore(CookieStore cookieStore) { this.cookieStore = cookieStore; }
    public final synchronized CredentialsProvider getCredentialsProvider() {
        if (credsProvider == null) credsProvider = new BasicCredentialsProvider();
        return credsProvider;
    }
    public synchronized void setCredentialsProvider(CredentialsProvider credsProvider) { this.credsProvider = credsProvider; }
    public final synchronized HttpRequestRetryHandler getHttpRequestRetryHandler() {
        if (retryHandler == null) retryHandler = new DefaultHttpRequestRetryHandler();
        return retryHandler;
    }
    public synchronized void setHttpRequestRetryHandler(HttpRequestRetryHandler retryHandler) { this.retryHandler = retryHandler; }
    public final synchronized RedirectHandler getRedirectHandler() {
        if (redirectHandler == null) redirectHandler = new DefaultRedirectHandler();
        return redirectHandler;
    }
    public synchronized void setRedirectHandler(RedirectHandler redirectHandler) { this.redirectHandler = redirectHandler; }
    public synchronized void addRequestInterceptor(HttpRequestInterceptor itcp) { if (itcp != null) requestInterceptors.add(itcp); }
    public synchronized void addRequestInterceptor(HttpRequestInterceptor itcp, int index) { if (itcp != null) requestInterceptors.add(index, itcp); }
    public synchronized void addResponseInterceptor(HttpResponseInterceptor itcp) { if (itcp != null) responseInterceptors.add(itcp); }
    public synchronized void addResponseInterceptor(HttpResponseInterceptor itcp, int index) { if (itcp != null) responseInterceptors.add(index, itcp); }
    public synchronized void clearRequestInterceptors() { requestInterceptors.clear(); }
    public synchronized void clearResponseInterceptors() { responseInterceptors.clear(); }
    public synchronized int getRequestInterceptorCount() { return requestInterceptors.size(); }
    public synchronized int getResponseInterceptorCount() { return responseInterceptors.size(); }

    @Override public final HttpResponse execute(HttpUriRequest request) throws IOException { return execute(request, (HttpContext) null); }
    @Override public final HttpResponse execute(HttpHost target, HttpRequest request) throws IOException { return execute(target, request, null); }
    @Override public final HttpResponse execute(HttpHost target, HttpRequest request, HttpContext context) throws IOException {
        if (request instanceof HttpUriRequest r) {
            if (target != null && r.getURI() != null && !r.getURI().isAbsolute() && r instanceof org.apache.http.client.methods.HttpRequestBase b)
                b.setURI(URI.create(target.toURI()).resolve(r.getURI()));
            return execute(r, context);
        }
        throw new ClientProtocolException("Only requests with a URI are supported");
    }
    @Override public <T> T execute(HttpUriRequest request, ResponseHandler<? extends T> responseHandler) throws IOException {
        return execute(request, responseHandler, null);
    }
    @Override public <T> T execute(HttpUriRequest request, ResponseHandler<? extends T> responseHandler, HttpContext context)
        throws IOException {
        HttpResponse response = execute(request, context);
        try {
            return responseHandler.handleResponse(response);
        } finally {
            HttpEntity entity = response.getEntity();
            if (entity != null) entity.consumeContent();
        }
    }

    @Override public final HttpResponse execute(HttpUriRequest request, HttpContext context) throws IOException {
        if (request == null) throw new IllegalArgumentException("Request must not be null.");
        HttpContext ctx = context != null ? context : new BasicHttpContext();
        ctx.setAttribute("http.request", request);
        ctx.setAttribute("http.cookie-store", getCookieStore());
        ctx.setAttribute("http.auth.credentials-provider", getCredentialsProvider());
        List<HttpRequestInterceptor> requestItcps;
        List<HttpResponseInterceptor> responseItcps;
        synchronized (this) {
            requestItcps = new ArrayList<>(requestInterceptors);
            responseItcps = new ArrayList<>(responseInterceptors);
        }
        try {
            for (HttpRequestInterceptor i : requestItcps) i.process(request, ctx);
        } catch (HttpException e) {
            throw new ClientProtocolException(e);
        }
        for (int attempt = 1;; ++attempt) {
            if (request.isAborted()) throw new IOException("Request already aborted");
            try {
                HttpResponse response = send(request, ctx);
                ctx.setAttribute("http.response", response);
                try {
                    for (HttpResponseInterceptor i : responseItcps) i.process(response, ctx);
                } catch (HttpException e) {
                    throw new ClientProtocolException(e);
                }
                return response;
            } catch (IOException e) {
                if (request.isAborted() || !getHttpRequestRetryHandler().retryRequest(e, attempt, ctx)) throw e;
            }
        }
    }

    private HttpResponse send(HttpUriRequest request, HttpContext ctx) throws IOException {
        URI uri = request.getURI();
        if (uri == null || !uri.isAbsolute()) throw new ClientProtocolException("URI does not specify a valid host name: " + uri);
        HttpParams params = request.getParams();
        HttpParams client = getParams();
        HttpURLConnection connection = (HttpURLConnection) uri.toURL().openConnection();
        connection.setRequestMethod(request.getMethod());
        int connect = HttpConnectionParams.getConnectionTimeout(params);
        int read = HttpConnectionParams.getSoTimeout(params);
        connection.setConnectTimeout(connect > 0 ? connect : HttpConnectionParams.getConnectionTimeout(client));
        connection.setReadTimeout(read > 0 ? read : HttpConnectionParams.getSoTimeout(client));
        connection.setInstanceFollowRedirects(params.getBooleanParameter("http.protocol.handle-redirects",
                                                                         client.getBooleanParameter("http.protocol.handle-redirects", true)));
        connection.setUseCaches(false);
        if (connection instanceof HttpsURLConnection https) {
            SchemeRegistry schemes = getConnectionManager() != null ? getConnectionManager().getSchemeRegistry() : null;
            org.apache.http.conn.scheme.Scheme scheme = schemes != null ? schemes.get("https") : null;
            if (scheme != null && scheme.getSocketFactory() instanceof org.apache.http.conn.ssl.SSLSocketFactory f
                && f.getHostnameVerifier() == org.apache.http.conn.ssl.SSLSocketFactory.ALLOW_ALL_HOSTNAME_VERIFIER)
                https.setHostnameVerifier((host, session) -> true);
        }
        String agent = HttpProtocolParams.getUserAgent(params);
        if (agent == null) agent = HttpProtocolParams.getUserAgent(client);
        if (agent != null) connection.setRequestProperty("User-Agent", agent);
        for (Header h : request.getAllHeaders()) connection.addRequestProperty(h.getName(), h.getValue());
        CookieOrigin origin = new CookieOrigin(uri.getHost() != null ? uri.getHost() : "", Math.max(uri.getPort(), 0),
                                               uri.getPath() != null && !uri.getPath().isEmpty() ? uri.getPath() : "/",
                                               "https".equalsIgnoreCase(uri.getScheme()));
        CookieSpec spec = new org.apache.http.impl.cookie.BrowserCompatSpec();
        if (!request.containsHeader("Cookie")) {
            ArrayList<Cookie> matched = new ArrayList<>();
            Date now = new Date();
            for (Cookie c : getCookieStore().getCookies())
                if (!c.isExpired(now) && spec.match(c, origin)) matched.add(c);
            if (!matched.isEmpty())
                for (Header h : spec.formatCookies(matched)) connection.addRequestProperty(h.getName(), h.getValue());
        }
        if (request instanceof HttpEntityEnclosingRequest enclosing && enclosing.getEntity() != null) {
            HttpEntity entity = enclosing.getEntity();
            connection.setDoOutput(true);
            if (entity.getContentType() != null && !request.containsHeader("Content-Type"))
                connection.setRequestProperty("Content-Type", entity.getContentType().getValue());
            if (entity.getContentEncoding() != null && !request.containsHeader("Content-Encoding"))
                connection.setRequestProperty("Content-Encoding", entity.getContentEncoding().getValue());
            long length = entity.getContentLength();
            if (length >= 0) connection.setFixedLengthStreamingMode(length);
            else connection.setChunkedStreamingMode(0);
            try (OutputStream out = connection.getOutputStream()) { entity.writeTo(out); }
        }
        int code = connection.getResponseCode();
        if (code < 0) throw new org.apache.http.NoHttpResponseException("The target server failed to respond");
        BasicHttpResponse response = new BasicHttpResponse(HttpVersion.HTTP_1_1, code, connection.getResponseMessage());
        for (Map.Entry<String, List<String>> e : connection.getHeaderFields().entrySet()) {
            if (e.getKey() == null) continue;
            for (String v : e.getValue()) {
                Header h = new BasicHeader(e.getKey(), v);
                response.addHeader(h);
                if (e.getKey().equalsIgnoreCase("Set-Cookie")) {
                    try {
                        for (Cookie c : spec.parse(h, origin)) getCookieStore().addCookie(c);
                    } catch (org.apache.http.cookie.MalformedCookieException ex) {
                        // a cookie that does not parse is left out, as Apache's client does
                    }
                }
            }
        }
        InputStream body = code >= 400 ? connection.getErrorStream() : connection.getInputStream();
        if (body != null && !"HEAD".equals(request.getMethod())) {
            BasicHttpEntity entity = new BasicHttpEntity();
            entity.setContent(new EofSensorInputStream(body));
            entity.setContentLength(connection.getContentLengthLong());
            if (connection.getContentType() != null) entity.setContentType(connection.getContentType());
            if (connection.getContentEncoding() != null) entity.setContentEncoding(connection.getContentEncoding());
            response.setEntity(entity);
        }
        return response;
    }
}
