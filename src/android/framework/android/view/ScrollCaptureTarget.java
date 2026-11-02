// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A view that can be scroll-captured. */
public final class ScrollCaptureTarget {
    private final View view;
    private final android.graphics.Rect localVisibleRect;
    private final android.graphics.Point positionInWindow;
    private final ScrollCaptureCallback callback;

    public ScrollCaptureTarget(View scrollTarget, android.graphics.Rect localVisibleRect, android.graphics.Point positionInWindow, ScrollCaptureCallback callback) {
        view = scrollTarget;
        this.localVisibleRect = localVisibleRect;
        this.positionInWindow = positionInWindow;
        this.callback = callback;
    }
    public View getContainingView() { return view; }
    public android.graphics.Rect getLocalVisibleRect() { return localVisibleRect; }
    public android.graphics.Point getPositionInWindow() { return positionInWindow; }
    public ScrollCaptureCallback getCallback() { return callback; }
    public void setScrollBounds(android.graphics.Rect scrollBounds) {}
    public int getHint() { return 0; }
}
