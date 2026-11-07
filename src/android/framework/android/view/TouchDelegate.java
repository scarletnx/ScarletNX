// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2008 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view;

import android.graphics.Rect;
import android.graphics.Region;
import android.view.accessibility.AccessibilityNodeInfo;

/** Gives a view the touches in an area of its parent that is larger than the view. */
public class TouchDelegate {
    public static final int ABOVE = 1, BELOW = 2, TO_LEFT = 4, TO_RIGHT = 8;
    private final Rect bounds, slopBounds;
    private final View delegateView;
    private boolean delegateTargeted;

    public TouchDelegate(Rect bounds, View delegateView) {
        this.bounds = bounds;
        this.delegateView = delegateView;
        int slop = ViewConfiguration.get(delegateView.getContext()).getScaledTouchSlop();
        slopBounds = new Rect(bounds);
        slopBounds.inset(-slop, -slop);
    }

    public boolean onTouchEvent(MotionEvent event) {
        int x = (int) event.getX(), y = (int) event.getY();
        boolean send = false, inside = true;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                delegateTargeted = bounds.contains(x, y);
                send = delegateTargeted;
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_MOVE:
                send = delegateTargeted;
                if (send) inside = slopBounds.contains(x, y);
                break;
            case MotionEvent.ACTION_CANCEL:
                send = delegateTargeted;
                delegateTargeted = false;
                break;
        }
        if (!send) return false;
        MotionEvent e = MotionEvent.obtain(event);
        if (inside) e.offsetLocation(-delegateView.getLeft(), -delegateView.getTop());
        else e.setLocation(delegateView.getWidth() / 2f, delegateView.getHeight() / 2f);
        return delegateView.dispatchTouchEvent(e);
    }
    public boolean onTouchExplorationHoverEvent(MotionEvent event) { return false; }
    public AccessibilityNodeInfo.TouchDelegateInfo getTouchDelegateInfo() {
        java.util.Map<Region, View> map = new java.util.HashMap<>();
        map.put(new Region(bounds), delegateView);
        return new AccessibilityNodeInfo.TouchDelegateInfo(map);
    }
}
