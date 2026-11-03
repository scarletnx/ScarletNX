// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.adid;

import android.content.Context;
import android.os.OutcomeReceiver;
import java.util.concurrent.Executor;

/** The advertising ID (Off: no ad services). Each call ends in onError with IllegalStateException. */
public class AdIdManager {
    private AdIdManager() {}
    public static AdIdManager get(Context context) { return new AdIdManager(); }
    public void getAdId(Executor executor, OutcomeReceiver<AdId, Exception> callback) { unavailable(executor, callback); }
    private static <R> void unavailable(Executor executor, OutcomeReceiver<R, Exception> receiver) {
        if (receiver == null) return;
        Runnable r = () -> receiver.onError(new IllegalStateException("AdServices is not available"));
        if (executor != null) executor.execute(r); else r.run();
    }
}
