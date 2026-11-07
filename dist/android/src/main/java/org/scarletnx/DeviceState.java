// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.PowerManager;

/** The launcher log's state: line at Log level 2 (src/gui/main.c), as the Apple apps' (src/gui/apple.m): the thermal
    status and the battery saver, said at the start and at each change of the line. The thermal status keeps Android's
    seven names, which Apple's four (nominal, fair, serious, critical) do not match one to one */
final class DeviceState {
    /** PowerManager's THERMAL_STATUS_*, by their number */
    private static final String[] THERMAL = {"none", "light", "moderate", "severe", "critical", "emergency",
                                             "shutdown"};

    /** where the lines go */
    interface Sink {
        void say(String line);
    }

    private final PowerManager power;
    private final int sdk;
    private final Sink sink;
    private String said;

    /** the state of power on a system of API level sdk, its lines to sink */
    DeviceState(PowerManager power, int sdk, Sink sink) {
        this.power = power;
        this.sdk = sdk;
        this.sink = sink;
    }

    /** a THERMAL_STATUS_*'s name, its number for one past those Android 10 has */
    static String thermalName(int status) {
        return status >= 0 && status < THERMAL.length ? THERMAL[status] : "status " + status;
    }

    /** the line, on a system of API level sdk: below Android 10 the system gives no thermal status */
    static String line(int sdk, int thermal, boolean saver) {
        String heat = sdk >= 29 ? thermalName(thermal) : "not reported by Android below 10 (API " + sdk + ")";
        return "state: thermal " + heat + ", low power mode " + (saver ? "on" : "off");
    }

    /** the line now, to the sink when it differs from the one said last */
    synchronized void look() {
        String now = line(sdk, sdk >= 29 ? power.getCurrentThermalStatus() : -1, power.isPowerSaveMode());
        if (now.equals(said)) return;
        said = now;
        sink.say(now);
    }

    /** the line now and at each change, for as long as context lives (the application's: the process) */
    void watch(Context context) {
        look();
        if (sdk >= 29) power.addThermalStatusListener(status -> look());
        BroadcastReceiver saver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context at, Intent intent) {
                look();
            }
        };
        IntentFilter filter = new IntentFilter(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED);
        if (sdk >= 33)
            context.registerReceiver(saver, filter, Context.RECEIVER_EXPORTED);
        else
            context.registerReceiver(saver, filter);
    }
}
