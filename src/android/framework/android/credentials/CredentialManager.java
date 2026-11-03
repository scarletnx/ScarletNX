// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

import android.content.Context;
import android.os.CancellationSignal;
import android.os.OutcomeReceiver;
import java.util.concurrent.Executor;

/**
 * The credential providers of the user. There are none: a retrieval ends in TYPE_NO_CREDENTIAL, a creation in
 * TYPE_NO_CREATE_OPTIONS, a prepared retrieval has no results, and a state clear succeeds with nothing to clear.
 */
public final class CredentialManager {
    public CredentialManager() {}

    public void getCredential(Context context, GetCredentialRequest request, CancellationSignal cancellationSignal,
            Executor executor, OutcomeReceiver<GetCredentialResponse, GetCredentialException> callback) {
        noCredential(executor, callback);
    }

    public void getCredential(Context context, PrepareGetCredentialResponse.PendingGetCredentialHandle pendingGetCredentialHandle,
            CancellationSignal cancellationSignal, Executor executor, OutcomeReceiver<GetCredentialResponse, GetCredentialException> callback) {
        noCredential(executor, callback);
    }

    public void prepareGetCredential(GetCredentialRequest request, CancellationSignal cancellationSignal, Executor executor,
            OutcomeReceiver<PrepareGetCredentialResponse, GetCredentialException> callback) {
        executor.execute(() -> callback.onResult(new PrepareGetCredentialResponse()));
    }

    public void createCredential(Context context, CreateCredentialRequest request, CancellationSignal cancellationSignal,
            Executor executor, OutcomeReceiver<CreateCredentialResponse, CreateCredentialException> callback) {
        executor.execute(() -> callback.onError(new CreateCredentialException(CreateCredentialException.TYPE_NO_CREATE_OPTIONS, "No credential provider")));
    }

    public void clearCredentialState(ClearCredentialStateRequest request, CancellationSignal cancellationSignal, Executor executor,
            OutcomeReceiver<Void, ClearCredentialStateException> callback) {
        executor.execute(() -> callback.onResult(null));
    }

    private static void noCredential(Executor executor, OutcomeReceiver<GetCredentialResponse, GetCredentialException> callback) {
        executor.execute(() -> callback.onError(new GetCredentialException(GetCredentialException.TYPE_NO_CREDENTIAL, "No credentials available")));
    }
}
