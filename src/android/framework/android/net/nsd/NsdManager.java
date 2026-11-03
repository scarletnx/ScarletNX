// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.nsd;

import android.os.Handler;
import android.os.Looper;

/** Network service discovery. No mDNS responder is here: each registration fails with an internal error. */
public final class NsdManager {
    public static final int PROTOCOL_DNS_SD = 1;
    public static final int FAILURE_INTERNAL_ERROR = 0, FAILURE_ALREADY_ACTIVE = 3, FAILURE_MAX_LIMIT = 4,
        FAILURE_OPERATION_NOT_RUNNING = 5, FAILURE_BAD_PARAMETERS = 6;
    public static final String ACTION_NSD_STATE_CHANGED = "android.net.nsd.STATE_CHANGED", EXTRA_NSD_STATE = "nsd_state";
    public static final int NSD_STATE_DISABLED = 1, NSD_STATE_ENABLED = 2;

    public interface RegistrationListener {
        void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onServiceRegistered(NsdServiceInfo serviceInfo);
        void onServiceUnregistered(NsdServiceInfo serviceInfo);
    }

    private final Handler handler = new Handler(Looper.getMainLooper());

    public NsdManager() {}

    public void registerService(NsdServiceInfo serviceInfo, int protocolType, RegistrationListener listener) {
        if (serviceInfo.getServiceName() == null || serviceInfo.getServiceName().isEmpty()) throw new IllegalArgumentException("Service name cannot be empty");
        if (serviceInfo.getServiceType() == null || serviceInfo.getServiceType().isEmpty()) throw new IllegalArgumentException("Service type cannot be empty");
        if (serviceInfo.getPort() <= 0) throw new IllegalArgumentException("Invalid port number");
        if (protocolType != PROTOCOL_DNS_SD) throw new IllegalArgumentException("Unsupported protocol");
        if (listener == null) throw new IllegalArgumentException("listener cannot be null");
        handler.post(() -> listener.onRegistrationFailed(serviceInfo, FAILURE_INTERNAL_ERROR));
    }

    /** A failed registration leaves the listener unregistered, so this listener is unknown. */
    public void unregisterService(RegistrationListener listener) {
        if (listener == null) throw new IllegalArgumentException("listener cannot be null");
        throw new IllegalArgumentException("listener not registered");
    }
}
