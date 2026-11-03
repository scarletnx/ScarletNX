// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** The callback of an operation that ends with a result or an error. */
public interface OutcomeReceiver<R, E extends Throwable> {
    void onResult(R result);

    default void onError(E error) {}
}
