// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media.tv;

import android.os.Handler;

/** TV inputs on a device that is no TV: there are none. */
public final class TvInputManager {
    public static final int INPUT_STATE_CONNECTED = 0, INPUT_STATE_CONNECTED_STANDBY = 1, INPUT_STATE_DISCONNECTED = 2;
    public static final String ACTION_SETUP_INPUTS = "android.media.tv.action.SETUP_INPUTS";

    public TvInputManager() {}

    public int getInputState(String inputId) { return INPUT_STATE_DISCONNECTED; }
    public boolean isParentalControlsEnabled() { return false; }
}
