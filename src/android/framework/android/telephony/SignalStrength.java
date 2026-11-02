// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.telephony;

/** A cell signal's strength: none */
public class SignalStrength {
    public static final int SIGNAL_STRENGTH_NONE_OR_UNKNOWN = 0;
    SignalStrength() {}
    public int getLevel() { return SIGNAL_STRENGTH_NONE_OR_UNKNOWN; }
    public boolean isGsm() { return false; }
}
