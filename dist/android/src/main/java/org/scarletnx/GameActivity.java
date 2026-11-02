// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** A game: the runtime's main in libscarletnx.so, with the menu's arguments */
public class GameActivity extends SDLActivity {
    /** The charger coming or going, for TV mode Auto: the runtime marks the change, and its main thread asks the
     *  system for the new state */
    private final BroadcastReceiver powerReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            nativePowerChanged();
        }
    };
    private boolean powerReceiverRegistered;

    private native void nativePowerChanged();

    /** HOME as the menu sets it (src/gui/android.c): a new process has none, and the keys are in its .switch/ */
    @Override
    protected void onCreate(Bundle state) {
        try {
            Os.setenv("HOME", getFilesDir().getPath(), true);
        } catch (ErrnoException e) {
            // the runtime then reads the keys from the data folder only
        }
        super.onCreate(state);
        registerPowerReceiver();
    }

    private void registerPowerReceiver() {
        IntentFilter filter = new IntentFilter();
        filter.addAction(Intent.ACTION_POWER_CONNECTED);
        filter.addAction(Intent.ACTION_POWER_DISCONNECTED);
        if (Build.VERSION.SDK_INT >= 33)
            registerReceiver(powerReceiver, filter, Context.RECEIVER_EXPORTED);
        else
            registerReceiver(powerReceiver, filter);
        powerReceiverRegistered = true;
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"scarletnx"};
    }

    @Override
    protected String getMainFunction() {
        return "main";
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return MainActivity.clearOfBars(context);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) MainActivity.hideBars(mSurface);
    }

    @Override
    protected String[] getArguments() {
        String[] args = getIntent().getStringArrayExtra("args");
        return args != null ? args : new String[0];
    }

    /** src/nx/runtime/platform/backend/sdl3.c: a stopped game's Share Log, the saved file to the app the user picks */
    public void share(String path) {
        MainActivity.share(this, path);
    }

    /** the runtime starts once per process: the next game gets a new one */
    @Override
    protected void onDestroy() {
        if (powerReceiverRegistered) unregisterReceiver(powerReceiver);
        super.onDestroy();
        System.exit(0);
    }
}
