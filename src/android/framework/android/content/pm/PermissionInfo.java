// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** A permission. */
public class PermissionInfo extends PackageItemInfo {
    public static final int PROTECTION_NORMAL = 0, PROTECTION_DANGEROUS = 1, PROTECTION_SIGNATURE = 2;
    public int protectionLevel;
    public String group;
}
