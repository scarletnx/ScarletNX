// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** The device's one user, unlocked. */
public class UserManager {
    public UserManager() {}
    public boolean isUserUnlocked() { return true; }
    public boolean isUserUnlocked(UserHandle user) { return true; }
    public boolean isDemoUser() { return false; }
    public boolean isManagedProfile() { return false; }
    public boolean isSystemUser() { return true; }
    public boolean isUserAGoat() { return false; }
    public boolean isUserRunning(UserHandle user) { return true; }
    public UserHandle getUserForSerialNumber(long serial) { return UserHandle.OWNER; }
    public long getSerialNumberForUser(UserHandle user) { return 0; }
    public java.util.List<UserHandle> getUserProfiles() { return java.util.List.of(UserHandle.OWNER); }
    public Bundle getUserRestrictions() { return new Bundle(); }
    public boolean hasUserRestriction(String key) { return false; }
    public static boolean supportsMultipleUsers() { return false; }
}
