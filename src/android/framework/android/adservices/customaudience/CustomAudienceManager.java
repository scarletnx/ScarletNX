// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.customaudience;

import android.content.Context;
import android.os.OutcomeReceiver;
import java.util.concurrent.Executor;

/** Custom audiences (Off: no ad services). Each call ends in onError with IllegalStateException. */
public class CustomAudienceManager {
    private CustomAudienceManager() {}
    public static CustomAudienceManager get(Context context) { return new CustomAudienceManager(); }
    public void joinCustomAudience(JoinCustomAudienceRequest joinCustomAudienceRequest, Executor executor, OutcomeReceiver<Object, Exception> receiver) { unavailable(executor, receiver); }
    public void leaveCustomAudience(LeaveCustomAudienceRequest leaveCustomAudienceRequest, Executor executor, OutcomeReceiver<Object, Exception> receiver) { unavailable(executor, receiver); }
    private static <R> void unavailable(Executor executor, OutcomeReceiver<R, Exception> receiver) {
        if (receiver == null) return;
        Runnable r = () -> receiver.onError(new IllegalStateException("AdServices is not available"));
        if (executor != null) executor.execute(r); else r.run();
    }
}
