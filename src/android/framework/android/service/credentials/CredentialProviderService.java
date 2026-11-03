// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.app.Service;
import android.content.Intent;
import android.credentials.ClearCredentialStateException;
import android.credentials.CreateCredentialException;
import android.credentials.GetCredentialException;
import android.os.CancellationSignal;
import android.os.IBinder;
import android.os.OutcomeReceiver;

/** A credential provider of an app. No credential manager calls it: no other app asks for credentials. */
public abstract class CredentialProviderService extends Service {
    public static final String SERVICE_INTERFACE = "android.service.credentials.CredentialProviderService";
    public static final String SERVICE_META_DATA = "android.credentials.provider";
    public static final String EXTRA_GET_CREDENTIAL_REQUEST = "android.service.credentials.extra.GET_CREDENTIAL_REQUEST";
    public static final String EXTRA_CREATE_CREDENTIAL_REQUEST = "android.service.credentials.extra.CREATE_CREDENTIAL_REQUEST";

    public CredentialProviderService() {}
    @Override public void onCreate() { super.onCreate(); }
    @Override public final IBinder onBind(Intent intent) { return null; }

    public abstract void onBeginGetCredential(BeginGetCredentialRequest request, CancellationSignal cancellationSignal,
        OutcomeReceiver<BeginGetCredentialResponse, GetCredentialException> callback);
    public abstract void onBeginCreateCredential(BeginCreateCredentialRequest request, CancellationSignal cancellationSignal,
        OutcomeReceiver<BeginCreateCredentialResponse, CreateCredentialException> callback);
    public abstract void onClearCredentialState(ClearCredentialStateRequest request, CancellationSignal cancellationSignal,
        OutcomeReceiver<Void, ClearCredentialStateException> callback);
}
