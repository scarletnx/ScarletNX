// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

/** Media routes: the only route is the device's own speaker, and no other route is ever found. */
public class MediaRouter {
    public static final int ROUTE_TYPE_LIVE_AUDIO = 1, ROUTE_TYPE_LIVE_VIDEO = 2, ROUTE_TYPE_USER = 0x800000;
    public static final int CALLBACK_FLAG_PERFORM_ACTIVE_SCAN = 1, CALLBACK_FLAG_UNFILTERED_EVENTS = 2;

    public MediaRouter() {}
}
