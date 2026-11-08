// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

import android.content.Intent;
import android.os.CancellationSignal;
import android.os.IBinder;

/**
 * An app's autofill service (Off in docs/android/INTERFACES.md: the system asks no service to fill other apps'
 * views here), so no request reaches it; it is a service the app may start as any other.
 */
public abstract class AutofillService extends android.app.Service {
    public static final String SERVICE_INTERFACE = "android.service.autofill.AutofillService",
        SERVICE_META_DATA = "android.autofill";

    public AutofillService() {}
    @Override public final IBinder onBind(Intent intent) { return null; }
    public void onConnected() {}
    public abstract void onFillRequest(FillRequest request, CancellationSignal cancellationSignal, FillCallback callback);
    public abstract void onSaveRequest(SaveRequest request, SaveCallback callback);
    public void onSavedDatasetsInfoRequest(SavedDatasetsInfoCallback callback) { callback.onError(SavedDatasetsInfoCallback.ERROR_UNSUPPORTED); }
    public void onDisconnected() {}
    public final FillEventHistory getFillEventHistory() { return null; }
}
