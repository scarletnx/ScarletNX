// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** A permission. */
public class PermissionInfo extends PackageItemInfo {
    public static final int PROTECTION_NORMAL = 0, PROTECTION_DANGEROUS = 1, PROTECTION_SIGNATURE = 2;
    public int protectionLevel;
    public String group;
    public static final int PROTECTION_MASK_BASE = 0xf, PROTECTION_MASK_FLAGS = 0xfff0;
    public int getProtection() { return protectionLevel & PROTECTION_MASK_BASE; }
    public int getProtectionFlags() { return protectionLevel & ~PROTECTION_MASK_BASE; }
}
