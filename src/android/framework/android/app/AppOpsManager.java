// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** App operations: each allowed. */
public class AppOpsManager {
    public static final int MODE_ALLOWED = 0, MODE_IGNORED = 1, MODE_ERRORED = 2, MODE_DEFAULT = 3;
    public static String permissionToOp(String permission) { return null; }
    public int noteOpNoThrow(String op, int uid, String packageName) { return MODE_ALLOWED; }
    public int noteProxyOp(String op, String proxiedPackageName) { return MODE_ALLOWED; }
    public int noteOpNoThrow(String op, int uid, String packageName, String attributionTag, String message) { return MODE_ALLOWED; }
    public int noteProxyOpNoThrow(String op, String proxiedPackageName) { return MODE_ALLOWED; }
    public int noteProxyOpNoThrow(String op, String proxiedPackageName, int proxiedUid) { return MODE_ALLOWED; }
    public int unsafeCheckOpNoThrow(String op, int uid, String packageName) { return MODE_ALLOWED; }
    public int checkOpNoThrow(String op, int uid, String packageName) { return MODE_ALLOWED; }
    public void checkPackage(int uid, String packageName) {}
}
