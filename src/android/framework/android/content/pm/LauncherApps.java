// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

import android.os.UserHandle;
import java.util.ArrayList;
import java.util.List;

/** The launcher's view of the apps: this app is no launcher, so it sees no activities or shortcuts of others. */
public class LauncherApps {
    public LauncherApps() {}

    public List<UserHandle> getProfiles() { List<UserHandle> l = new ArrayList<>(); l.add(android.os.Process.myUserHandle()); return l; }
    public boolean hasShortcutHostPermission() { return false; }
    public boolean isPackageEnabled(String packageName, UserHandle user) { return false; }
}
