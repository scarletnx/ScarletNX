// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.topics;

import android.content.Context;
import android.os.OutcomeReceiver;
import java.util.concurrent.Executor;

/** The topics of the user's interests (Off: no ad services). Each call ends in onError with IllegalStateException. */
public final class TopicsManager {
    private TopicsManager() {}
    public static TopicsManager get(Context context) { return new TopicsManager(); }
    public void getTopics(GetTopicsRequest getTopicsRequest, Executor executor, OutcomeReceiver<GetTopicsResponse, Exception> callback) { unavailable(executor, callback); }
    private static <R> void unavailable(Executor executor, OutcomeReceiver<R, Exception> receiver) {
        if (receiver == null) return;
        Runnable r = () -> receiver.onError(new IllegalStateException("AdServices is not available"));
        if (executor != null) executor.execute(r); else r.run();
    }
}
