// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.accounts;

import java.io.IOException;
import java.util.concurrent.TimeUnit;

/** The result of an asynchronous AccountManager call. */
public interface AccountManagerFuture<V> {
    boolean cancel(boolean mayInterruptIfRunning);
    boolean isCancelled();
    boolean isDone();
    V getResult() throws OperationCanceledException, IOException, AuthenticatorException;
    V getResult(long timeout, TimeUnit unit) throws OperationCanceledException, IOException, AuthenticatorException;
}
