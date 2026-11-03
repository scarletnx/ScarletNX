// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media.projection;

import android.content.Intent;

/** Screen capture. No capture service is here: the intent that asks the user finds no activity to answer. */
public final class MediaProjectionManager {
    public MediaProjectionManager() {}

    public Intent createScreenCaptureIntent() {
        return new Intent().setClassName("com.android.systemui", "com.android.systemui.mediaprojection.permission.MediaProjectionPermissionActivity");
    }
}
