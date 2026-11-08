// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.ssl;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.security.KeyManagementException;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.NoSuchAlgorithmException;
import java.security.UnrecoverableKeyException;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManagerFactory;
import org.apache.http.conn.scheme.LayeredSocketFactory;
import org.apache.http.params.HttpConnectionParams;
import org.apache.http.params.HttpParams;

/** TLS sockets, from the JDK's SSLContext with the key and trust stores given (the JDK's own when null). */
public class SSLSocketFactory implements LayeredSocketFactory {
    public static final String TLS = "TLS", SSL = "SSL", SSLV2 = "SSLv2";
    public static final X509HostnameVerifier ALLOW_ALL_HOSTNAME_VERIFIER = new AllowAllHostnameVerifier(),
        BROWSER_COMPATIBLE_HOSTNAME_VERIFIER = new BrowserCompatHostnameVerifier(),
        STRICT_HOSTNAME_VERIFIER = new StrictHostnameVerifier();
    private static final SSLSocketFactory DEFAULT_FACTORY = new SSLSocketFactory();

    private final javax.net.ssl.SSLSocketFactory socketfactory;
    private X509HostnameVerifier hostnameVerifier = BROWSER_COMPATIBLE_HOSTNAME_VERIFIER;

    public static SSLSocketFactory getSocketFactory() { return DEFAULT_FACTORY; }

    public SSLSocketFactory(String algorithm, KeyStore keystore, String keystorePassword, KeyStore truststore,
                            java.security.SecureRandom random, org.apache.http.conn.scheme.HostNameResolver nameResolver)
        throws NoSuchAlgorithmException, KeyManagementException, KeyStoreException, UnrecoverableKeyException {
        SSLContext context = SSLContext.getInstance(algorithm != null ? algorithm : TLS);
        javax.net.ssl.KeyManager[] keys = null;
        if (keystore != null) {
            KeyManagerFactory kmf = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
            kmf.init(keystore, keystorePassword != null ? keystorePassword.toCharArray() : null);
            keys = kmf.getKeyManagers();
        }
        javax.net.ssl.TrustManager[] trust = null;
        if (truststore != null) {
            TrustManagerFactory tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            tmf.init(truststore);
            trust = tmf.getTrustManagers();
        }
        context.init(keys, trust, random);
        socketfactory = context.getSocketFactory();
    }
    public SSLSocketFactory(KeyStore keystore, String keystorePassword, KeyStore truststore)
        throws NoSuchAlgorithmException, KeyManagementException, KeyStoreException, UnrecoverableKeyException {
        this(TLS, keystore, keystorePassword, truststore, null, null);
    }
    public SSLSocketFactory(KeyStore keystore, String keystorePassword)
        throws NoSuchAlgorithmException, KeyManagementException, KeyStoreException, UnrecoverableKeyException {
        this(TLS, keystore, keystorePassword, null, null, null);
    }
    public SSLSocketFactory(KeyStore truststore)
        throws NoSuchAlgorithmException, KeyManagementException, KeyStoreException, UnrecoverableKeyException {
        this(TLS, null, null, truststore, null, null);
    }
    public SSLSocketFactory(javax.net.ssl.SSLSocketFactory socketfactory) { this.socketfactory = socketfactory; }
    private SSLSocketFactory() { socketfactory = (javax.net.ssl.SSLSocketFactory) javax.net.ssl.SSLSocketFactory.getDefault(); }

    @Override public Socket createSocket() throws IOException { return socketfactory.createSocket(); }
    @Override public Socket connectSocket(Socket sock, String host, int port, InetAddress localAddress, int localPort,
                                          HttpParams params) throws IOException {
        SSLSocket ssl = (SSLSocket) (sock != null ? sock : createSocket());
        if (localAddress != null || localPort > 0) ssl.bind(new InetSocketAddress(localAddress, Math.max(localPort, 0)));
        ssl.connect(new InetSocketAddress(host, port), params != null ? HttpConnectionParams.getConnectionTimeout(params) : 0);
        if (params != null) ssl.setSoTimeout(HttpConnectionParams.getSoTimeout(params));
        hostnameVerifier.verify(host, ssl);
        return ssl;
    }
    @Override public boolean isSecure(Socket sock) {
        if (sock == null) throw new IllegalArgumentException("Socket may not be null.");
        return sock instanceof SSLSocket && !sock.isClosed();
    }
    @Override public Socket createSocket(Socket socket, String host, int port, boolean autoClose) throws IOException {
        SSLSocket ssl = (SSLSocket) socketfactory.createSocket(socket, host, port, autoClose);
        hostnameVerifier.verify(host, ssl);
        return ssl;
    }
    public void setHostnameVerifier(X509HostnameVerifier hostnameVerifier) {
        if (hostnameVerifier == null) throw new IllegalArgumentException("Hostname verifier may not be null");
        this.hostnameVerifier = hostnameVerifier;
    }
    public X509HostnameVerifier getHostnameVerifier() { return hostnameVerifier; }
}
