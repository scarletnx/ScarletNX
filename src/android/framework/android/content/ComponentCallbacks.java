// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

/** Told of configuration changes and low memory. */
public interface ComponentCallbacks {
    void onConfigurationChanged(android.content.res.Configuration newConfig);
    @Deprecated void onLowMemory();
}
