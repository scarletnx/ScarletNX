// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.security;

import android.content.pm.ApplicationInfo;
import snx.android.app.Package;

/**
 * The network security policy of the app. Cleartext traffic follows Android's default: permitted when the manifest
 * sets usesCleartextTraffic or the app targets API 27 or lower. The domains of a network security config are not read.
 */
public class NetworkSecurityPolicy {
    private static final NetworkSecurityPolicy INSTANCE = new NetworkSecurityPolicy();

    private NetworkSecurityPolicy() {}
    public static NetworkSecurityPolicy getInstance() { return INSTANCE; }

    public boolean isCleartextTrafficPermitted() {
        ApplicationInfo app = Package.get().application;
        return (app.flags & ApplicationInfo.FLAG_USES_CLEARTEXT_TRAFFIC) != 0 || app.targetSdkVersion < 28;
    }

    public boolean isCleartextTrafficPermitted(String hostname) { return isCleartextTrafficPermitted(); }
}
