// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

import java.net.InetAddress;
import java.util.List;
import java.util.concurrent.Executor;

/** Name lookups, through the host's resolver; the answer or the error comes on the executor */
public final class DnsResolver {
    public static final int CLASS_IN = 1, TYPE_A = 1, TYPE_AAAA = 28, FLAG_EMPTY = 0, FLAG_NO_RETRY = 1,
        FLAG_NO_CACHE_STORE = 2, FLAG_NO_CACHE_LOOKUP = 4, ERROR_PARSE = 0, ERROR_SYSTEM = 1;
    private static final DnsResolver INSTANCE = new DnsResolver();

    public interface Callback<T> {
        void onAnswer(T answer, int rcode);
        void onError(DnsException error);
    }
    public static class DnsException extends Exception {
        public final int code;
        public DnsException(int code, Throwable cause) {
            super(cause);
            this.code = code;
        }
    }

    private DnsResolver() {}
    public static DnsResolver getInstance() { return INSTANCE; }

    public void query(Network network, String domain, int flags, Executor executor, android.os.CancellationSignal cancellationSignal,
                      Callback<? super List<InetAddress>> callback) {
        lookup(domain, -1, executor, callback);
    }
    public void query(Network network, String domain, int nsType, int flags, Executor executor,
                      android.os.CancellationSignal cancellationSignal, Callback<? super List<InetAddress>> callback) {
        lookup(domain, nsType, executor, callback);
    }
    private static void lookup(String domain, int type, Executor executor, Callback<? super List<InetAddress>> callback) {
        new Thread(() -> {
            try {
                java.util.ArrayList<InetAddress> out = new java.util.ArrayList<>();
                for (InetAddress a : InetAddress.getAllByName(domain)) {
                    boolean v6 = a instanceof java.net.Inet6Address;
                    if (type == -1 || (type == TYPE_AAAA) == v6) out.add(a);
                }
                executor.execute(() -> callback.onAnswer(out, 0));
            } catch (java.io.IOException e) {
                executor.execute(() -> callback.onError(new DnsException(ERROR_SYSTEM, e)));
            }
        }, "DnsResolver").start();
    }
}
