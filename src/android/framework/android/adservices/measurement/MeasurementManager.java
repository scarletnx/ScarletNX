// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.measurement;

import android.content.Context;
import android.net.Uri;
import android.os.OutcomeReceiver;
import android.view.InputEvent;
import java.util.concurrent.Executor;

/**
 * Attribution measurement (Off: no ad services). The API status is MEASUREMENT_API_STATE_DISABLED, and each other
 * call ends in onError with IllegalStateException.
 */
public class MeasurementManager {
    public static final int MEASUREMENT_API_STATE_DISABLED = 0, MEASUREMENT_API_STATE_ENABLED = 1;

    private MeasurementManager() {}
    public static MeasurementManager get(Context context) { return new MeasurementManager(); }
    public void registerSource(Uri attributionSource, InputEvent inputEvent, Executor executor, OutcomeReceiver<Object, Exception> callback) { unavailable(executor, callback); }
    public void registerTrigger(Uri trigger, Executor executor, OutcomeReceiver<Object, Exception> callback) { unavailable(executor, callback); }
    public void registerWebSource(WebSourceRegistrationRequest request, Executor executor, OutcomeReceiver<Object, Exception> callback) { unavailable(executor, callback); }
    public void registerWebTrigger(WebTriggerRegistrationRequest request, Executor executor, OutcomeReceiver<Object, Exception> callback) { unavailable(executor, callback); }
    public void deleteRegistrations(DeletionRequest deletionRequest, Executor executor, OutcomeReceiver<Object, Exception> callback) { unavailable(executor, callback); }

    public void getMeasurementApiStatus(Executor executor, OutcomeReceiver<Integer, Exception> callback) {
        executor.execute(() -> callback.onResult(MEASUREMENT_API_STATE_DISABLED));
    }

    private static <R> void unavailable(Executor executor, OutcomeReceiver<R, Exception> receiver) {
        if (receiver == null) return;
        Runnable r = () -> receiver.onError(new IllegalStateException("AdServices is not available"));
        if (executor != null) executor.execute(r); else r.run();
    }
}
