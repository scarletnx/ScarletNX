// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.role;

import android.content.Intent;
import android.os.UserHandle;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

/**
 * The roles of the device's default apps. The device has the roles, and the app holds none of them: a request for
 * one opens the system's dialog, which the user backs out of (snx.android.app.Activities answers RESULT_CANCELED).
 */
public final class RoleManager {
    public static final String ROLE_ASSISTANT = "android.app.role.ASSISTANT";
    public static final String ROLE_BROWSER = "android.app.role.BROWSER";
    public static final String ROLE_CALL_REDIRECTION = "android.app.role.CALL_REDIRECTION";
    public static final String ROLE_CALL_SCREENING = "android.app.role.CALL_SCREENING";
    public static final String ROLE_DIALER = "android.app.role.DIALER";
    public static final String ROLE_EMERGENCY = "android.app.role.EMERGENCY";
    public static final String ROLE_HOME = "android.app.role.HOME";
    public static final String ROLE_SMS = "android.app.role.SMS";
    public static final String ROLE_NOTES = "android.app.role.NOTES";
    public static final String ROLE_WALLET = "android.app.role.WALLET";
    public static final String ACTION_REQUEST_ROLE = "android.app.role.action.REQUEST_ROLE";
    public static final String EXTRA_ROLE_NAME = "android.app.role.extra.ROLE_NAME";
    private static final Set<String> ROLES = Set.of(ROLE_ASSISTANT, ROLE_BROWSER, ROLE_CALL_REDIRECTION, ROLE_CALL_SCREENING,
        ROLE_DIALER, ROLE_EMERGENCY, ROLE_HOME, ROLE_SMS, ROLE_NOTES, ROLE_WALLET);

    public RoleManager() {}

    public boolean isRoleAvailable(String roleName) { return ROLES.contains(roleName); }
    public boolean isRoleHeld(String roleName) { return false; }
    public Intent createRequestRoleIntent(String roleName) {
        if (roleName == null || roleName.isEmpty()) throw new IllegalArgumentException("roleName cannot be null or empty");
        return new Intent(ACTION_REQUEST_ROLE).setPackage("com.android.permissioncontroller").putExtra(EXTRA_ROLE_NAME, roleName);
    }
    public List<String> getRoleHolders(String roleName) { return List.of(); }
    public List<String> getRoleHoldersAsUser(String roleName, UserHandle user) { return List.of(); }
    public void addRoleHolderAsUser(String roleName, String packageName, int flags, UserHandle user, Executor executor, Consumer<Boolean> callback) {
        executor.execute(() -> callback.accept(false));
    }
}
