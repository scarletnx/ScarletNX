// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.telephony;

/** SIM subscriptions: none, as on a device without a SIM */
public class SubscriptionManager {
    public static final int INVALID_SUBSCRIPTION_ID = -1, INVALID_SIM_SLOT_INDEX = -1, DEFAULT_SUBSCRIPTION_ID = Integer.MAX_VALUE;
    public SubscriptionManager() {}
    public static int getSlotIndex(int subscriptionId) { return INVALID_SIM_SLOT_INDEX; }
    public static int getDefaultSubscriptionId() { return INVALID_SUBSCRIPTION_ID; }
    public static int getDefaultDataSubscriptionId() { return INVALID_SUBSCRIPTION_ID; }
    public static boolean isValidSubscriptionId(int subscriptionId) { return false; }
    public java.util.List<Object> getActiveSubscriptionInfoList() { return null; }
    public int getActiveSubscriptionInfoCount() { return 0; }
}
