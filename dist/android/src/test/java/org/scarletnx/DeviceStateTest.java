// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertEquals;
import static org.robolectric.Shadows.shadowOf;

import android.content.Context;
import android.content.Intent;
import android.os.Looper;
import android.os.PowerManager;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.shadows.ShadowPowerManager;

/** The launcher log's state: line (DeviceState), as the Apple apps' (src/gui/apple.m): Android's seven thermal
    statuses by their names, the battery saver as the low power mode, a line at the start and one at each change of
    it, and below Android 10 that the system gives no thermal status, never a status made up */
@RunWith(RobolectricTestRunner.class)
public class DeviceStateTest {
    private Context context;
    private PowerManager power;
    private ShadowPowerManager shadow;
    private final List<String> said = new ArrayList<>();

    @Before
    public void setUp() {
        context = RuntimeEnvironment.getApplication();
        power = (PowerManager)context.getSystemService(Context.POWER_SERVICE);
        shadow = shadowOf(power);
        shadow.setCurrentThermalStatus(PowerManager.THERMAL_STATUS_NONE);
        shadow.setIsPowerSaveMode(false);
    }

    @Test
    public void eachThermalStatusHasAndroidsName() {
        int[] statuses = {PowerManager.THERMAL_STATUS_NONE,     PowerManager.THERMAL_STATUS_LIGHT,
                          PowerManager.THERMAL_STATUS_MODERATE, PowerManager.THERMAL_STATUS_SEVERE,
                          PowerManager.THERMAL_STATUS_CRITICAL, PowerManager.THERMAL_STATUS_EMERGENCY,
                          PowerManager.THERMAL_STATUS_SHUTDOWN};
        String[] names = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
        for (int i = 0; i < statuses.length; ++i) assertEquals(names[i], DeviceState.thermalName(statuses[i]));
        assertEquals("status 7", DeviceState.thermalName(7)); // one a later Android may add: its number
        assertEquals("status -1", DeviceState.thermalName(-1));
    }

    @Test
    public void theLineIsTheApplesOne() {
        assertEquals("state: thermal severe, low power mode on", DeviceState.line(29, 3, true));
        assertEquals("state: thermal none, low power mode off", DeviceState.line(36, 0, false));
    }

    @Test
    public void belowAndroid10TheLineSaysTheSystemGivesNoThermalStatus() {
        assertEquals("state: thermal not reported by Android below 10 (API 28), low power mode on",
                     DeviceState.line(28, 3, true));
        shadow.setCurrentThermalStatus(PowerManager.THERMAL_STATUS_SEVERE);
        new DeviceState(power, 28, said::add).watch(context);
        assertEquals(Arrays.asList("state: thermal not reported by Android below 10 (API 28), low power mode off"),
                     said);
    }

    @Test
    public void theWatchSaysTheStateThenEachChangeOnce() {
        new DeviceState(power, 36, said::add).watch(context);
        assertEquals(Arrays.asList("state: thermal none, low power mode off"), said);
        shadow.setCurrentThermalStatus(PowerManager.THERMAL_STATUS_SEVERE);
        shadowOf(Looper.getMainLooper()).idle();
        shadow.setCurrentThermalStatus(PowerManager.THERMAL_STATUS_SEVERE); // the same again: no line
        shadowOf(Looper.getMainLooper()).idle();
        shadow.setIsPowerSaveMode(true);
        context.sendBroadcast(new Intent(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED));
        shadowOf(Looper.getMainLooper()).idle();
        context.sendBroadcast(new Intent(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED)); // nothing changed
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(Arrays.asList("state: thermal none, low power mode off",
                                   "state: thermal severe, low power mode off",
                                   "state: thermal severe, low power mode on"),
                     said);
    }
}
