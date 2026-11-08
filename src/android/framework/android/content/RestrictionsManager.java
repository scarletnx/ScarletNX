// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import android.os.Bundle;
import android.os.PersistableBundle;
import java.util.ArrayList;
import java.util.List;

/** Application restrictions: no device policy manages this device, so the app has none. */
public class RestrictionsManager {
    public static final String ACTION_PERMISSION_RESPONSE_RECEIVED = "android.content.action.PERMISSION_RESPONSE_RECEIVED",
        ACTION_REQUEST_PERMISSION = "android.content.action.REQUEST_PERMISSION",
        ACTION_REQUEST_LOCAL_APPROVAL = "android.content.action.REQUEST_LOCAL_APPROVAL";

    public RestrictionsManager() {}

    public Bundle getApplicationRestrictions() { return new Bundle(); }
    public List<Bundle> getApplicationRestrictionsPerAdmin() { return new ArrayList<>(); }
    public boolean hasRestrictionsProvider() { return false; }
    public void requestPermission(String requestType, String requestId, PersistableBundle request) {}
    public Intent createLocalApprovalIntent() { return null; }
    public void notifyPermissionResponse(String packageName, PersistableBundle response) {}
}
