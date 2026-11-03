// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.accounts;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import java.util.concurrent.TimeUnit;

/** The accounts of the device. There are none: each query answers an empty list, and tokens are not kept. */
public class AccountManager {
    public static final String KEY_ACCOUNT_NAME = "authAccount", KEY_ACCOUNT_TYPE = "accountType",
        KEY_AUTHTOKEN = "authtoken", KEY_INTENT = "intent", KEY_ERROR_CODE = "errorCode",
        KEY_ERROR_MESSAGE = "errorMessage";
    private static final AccountManager INSTANCE = new AccountManager();

    private AccountManager() {}
    public static AccountManager get(Context context) { return INSTANCE; }
    public Account[] getAccounts() { return new Account[0]; }
    public Account[] getAccountsByType(String type) { return new Account[0]; }

    public AccountManagerFuture<Account[]> getAccountsByTypeAndFeatures(String type, String[] features,
            AccountManagerCallback<Account[]> callback, Handler handler) {
        AccountManagerFuture<Account[]> future = new Done<>(new Account[0]);
        if (callback != null) new Handler(handler != null ? handler.getLooper() : Looper.getMainLooper()).post(() -> callback.run(future));
        return future;
    }

    public void invalidateAuthToken(String accountType, String authToken) {}

    private static final class Done<V> implements AccountManagerFuture<V> {
        private final V result;
        Done(V result) { this.result = result; }
        @Override public boolean cancel(boolean mayInterruptIfRunning) { return false; }
        @Override public boolean isCancelled() { return false; }
        @Override public boolean isDone() { return true; }
        @Override public V getResult() { return result; }
        @Override public V getResult(long timeout, TimeUnit unit) { return result; }
    }
}
