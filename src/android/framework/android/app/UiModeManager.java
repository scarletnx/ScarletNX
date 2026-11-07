// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** The UI mode: a normal device, night as the configuration says. */
public class UiModeManager {
    public static final int MODE_NIGHT_AUTO = 0, MODE_NIGHT_NO = 1, MODE_NIGHT_YES = 2, MODE_NIGHT_CUSTOM = 3;
    public int getCurrentModeType() { return android.content.res.Configuration.UI_MODE_TYPE_NORMAL; }
    public int getNightMode() { return snx.android.Device.configuration().isNightModeActive() ? MODE_NIGHT_YES : MODE_NIGHT_NO; }
    public void setNightMode(int mode) {}
    public void setApplicationNightMode(int mode) {}
    public float getContrast() { return 0; }
}
