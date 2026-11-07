// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

public abstract class PermissionRequest {
    public static final String RESOURCE_VIDEO_CAPTURE = "android.webkit.resource.VIDEO_CAPTURE",
        RESOURCE_AUDIO_CAPTURE = "android.webkit.resource.AUDIO_CAPTURE";
    public abstract android.net.Uri getOrigin();
    public abstract String[] getResources();
    public abstract void grant(String[] resources);
    public abstract void deny();
}
