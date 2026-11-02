// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.view.View;

/** A magnifier over text: not shown. */
public final class Magnifier {
    public static final int SOURCE_BOUND_MAX_IN_SURFACE = 0, SOURCE_BOUND_MAX_VISIBLE = 1;
    private final int width, height;

    public Magnifier(View view) { width = 100; height = 48; }
    public void show(float sourceCenterX, float sourceCenterY) {}
    public void show(float sourceCenterX, float sourceCenterY, float magnifierCenterX, float magnifierCenterY) {}
    public void dismiss() {}
    public void update() {}
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getSourceWidth() { return width; }
    public int getSourceHeight() { return height; }
    public float getZoom() { return 1.25f; }
    public void setZoom(float zoom) {}
    public android.graphics.Point getPosition() { return null; }
    public android.graphics.Point getSourcePosition() { return null; }

    public static final class Builder {
        private final View view;
        public Builder(View view) { this.view = view; }
        public Builder setSize(int width, int height) { return this; }
        public Builder setInitialZoom(float zoom) { return this; }
        public Builder setElevation(float elevation) { return this; }
        public Builder setCornerRadius(float cornerRadius) { return this; }
        public Builder setClippingEnabled(boolean clip) { return this; }
        public Builder setDefaultSourceToMagnifierOffset(int horizontalOffset, int verticalOffset) { return this; }
        public Builder setOverlay(android.graphics.drawable.Drawable overlay) { return this; }
        public Builder setSourceBounds(int left, int top, int right, int bottom) { return this; }
        public Magnifier build() { return new Magnifier(view); }
    }
}
