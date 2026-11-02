// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

@Deprecated
public final class CookieSyncManager {
    private static final CookieSyncManager instance = new CookieSyncManager();
    private CookieSyncManager() {}
    public static CookieSyncManager createInstance(android.content.Context context) { return instance; }
    public static CookieSyncManager getInstance() { return instance; }
    public void sync() {}
    public void startSync() {}
    public void stopSync() {}
}
