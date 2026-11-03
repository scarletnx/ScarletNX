// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.adselection;

import android.content.Context;
import android.os.OutcomeReceiver;
import java.util.concurrent.Executor;

/** On-device ad selection (Off: no ad services). Each call ends in onError with IllegalStateException. */
public class AdSelectionManager {
    private AdSelectionManager() {}
    public static AdSelectionManager get(Context context) { return new AdSelectionManager(); }
    public void selectAds(AdSelectionConfig adSelectionConfig, Executor executor, OutcomeReceiver<AdSelectionOutcome, Exception> receiver) { unavailable(executor, receiver); }
    public void reportImpression(ReportImpressionRequest request, Executor executor, OutcomeReceiver<Object, Exception> receiver) { unavailable(executor, receiver); }
    private static <R> void unavailable(Executor executor, OutcomeReceiver<R, Exception> receiver) {
        if (receiver == null) return;
        Runnable r = () -> receiver.onError(new IllegalStateException("AdServices is not available"));
        if (executor != null) executor.execute(r); else r.run();
    }
}
