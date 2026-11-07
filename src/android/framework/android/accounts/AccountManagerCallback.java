// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.accounts;

/** The callback of an asynchronous AccountManager call. */
public interface AccountManagerCallback<V> {
    void run(AccountManagerFuture<V> future);
}
