// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

/** Told when a bound service connects. */
public interface ServiceConnection {
    void onServiceConnected(ComponentName name, android.os.IBinder service);
    void onServiceDisconnected(ComponentName name);
    default void onBindingDied(ComponentName name) {}
    default void onNullBinding(ComponentName name) {}
}
