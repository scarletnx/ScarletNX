// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.telephony;

import java.util.concurrent.Executor;

/**
 * The phone: a device without one (Off in docs/android/INTERFACES.md), as a tablet without a modem reports it. The
 * device identifiers need a privileged permission since Android 10, so they throw for an app as there.
 */
public class TelephonyManager {
    public static final int PHONE_TYPE_NONE = 0, PHONE_TYPE_GSM = 1, PHONE_TYPE_CDMA = 2, PHONE_TYPE_SIP = 3;
    public static final int SIM_STATE_UNKNOWN = 0, SIM_STATE_ABSENT = 1, SIM_STATE_READY = 5;
    public static final int NETWORK_TYPE_UNKNOWN = 0, DATA_ACTIVITY_NONE = 0, DATA_DISCONNECTED = 0, CALL_STATE_IDLE = 0;

    public abstract static class CellInfoCallback {
        public static final int ERROR_TIMEOUT = 1, ERROR_MODEM_ERROR = 2;
        public CellInfoCallback() {}
        public abstract void onCellInfo(java.util.List<Object> cellInfo);
        public void onError(int errorCode, Throwable detail) {}
    }

    public TelephonyManager() {}

    private static SecurityException noPermission() {
        return new SecurityException("getDeviceId: The user " + android.os.Process.myUid()
            + " does not meet the requirements to access device identifiers.");
    }
    public String getDeviceId() { throw noPermission(); }
    public String getDeviceId(int slotIndex) { throw noPermission(); }
    public String getImei() { throw noPermission(); }
    public String getImei(int slotIndex) { throw noPermission(); }
    public String getSubscriberId() { throw noPermission(); }
    public String getLine1Number() { return null; }
    public int getPhoneType() { return PHONE_TYPE_NONE; }
    public int getSimState() { return SIM_STATE_ABSENT; }
    public int getSimState(int slotIndex) { return SIM_STATE_ABSENT; }
    public String getSimOperator() { return ""; }
    public String getSimOperatorName() { return ""; }
    public String getSimCountryIso() { return ""; }
    public String getNetworkOperator() { return ""; }
    public String getNetworkOperatorName() { return ""; }
    public String getNetworkCountryIso() { return ""; }
    public int getNetworkType() { return NETWORK_TYPE_UNKNOWN; }
    public int getDataNetworkType() { return NETWORK_TYPE_UNKNOWN; }
    public int getDataActivity() { return DATA_ACTIVITY_NONE; }
    public int getDataState() { return DATA_DISCONNECTED; }
    public int getCallState() { return CALL_STATE_IDLE; }
    public int getSubscriptionId() { return SubscriptionManager.INVALID_SUBSCRIPTION_ID; }
    public int getPhoneCount() { return 0; }
    public boolean isNetworkRoaming() { return false; }
    public boolean hasIccCard() { return false; }
    public boolean isVoiceCapable() { return false; }
    public boolean isSmsCapable() { return false; }
    public SignalStrength getSignalStrength() { return null; }
    public TelephonyManager createForSubscriptionId(int subId) { return this; }
    @Deprecated public void listen(PhoneStateListener listener, int events) {}
    public void requestCellInfoUpdate(Executor executor, CellInfoCallback callback) {
        executor.execute(() -> callback.onCellInfo(new java.util.ArrayList<>()));
    }
}
