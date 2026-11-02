// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.graphics.BlendMode;
import android.graphics.Canvas;
import android.util.AttributeSet;

/**
 * The overscroll of a scrolling edge, as Android 12's stretch: a distance that pulls grow and a release eases back
 * to zero over a few frames. The stretch itself is drawn by the scrolling view, from getDistance.
 */
public class EdgeEffect {
    public static final int TYPE_GLOW = 0, TYPE_STRETCH = 1;
    public static final BlendMode DEFAULT_BLEND_MODE = BlendMode.SRC_ATOP;
    private float distance, velocity;
    private int width, height, color = 0x33ffffff, type = TYPE_STRETCH;
    private long releaseTime = -1;
    private BlendMode blendMode = DEFAULT_BLEND_MODE;

    public EdgeEffect(Context context) {}
    public EdgeEffect(Context context, AttributeSet attrs) {}

    public void setSize(int width, int height) { this.width = width; this.height = height; }
    public void setType(int type) { this.type = type; }
    public int getType() { return type; }
    public boolean isFinished() { return distance == 0 && releaseTime < 0; }
    public void finish() { distance = 0; releaseTime = -1; }
    public void onPull(float deltaDistance) { onPullDistance(deltaDistance, 0.5f); }
    public void onPull(float deltaDistance, float displacement) { onPullDistance(deltaDistance, displacement); }
    public float onPullDistance(float deltaDistance, float displacement) {
        float before = distance;
        distance = Math.max(0, Math.min(1, distance + deltaDistance));
        releaseTime = -1;
        return distance - before;
    }
    public float getDistance() {
        if (releaseTime >= 0) {
            float t = (android.view.animation.AnimationUtils.currentAnimationTimeMillis() - releaseTime) / 300f;
            if (t >= 1) { distance = 0; releaseTime = -1; }
            else return distance * (1 - t) * (1 - t);
        }
        return distance;
    }
    public void onRelease() { if (distance > 0 && releaseTime < 0) releaseTime = android.view.animation.AnimationUtils.currentAnimationTimeMillis(); }
    public void onAbsorb(int velocity) { this.velocity = velocity; }
    public void setColor(int color) { this.color = color; }
    public int getColor() { return color; }
    public void setBlendMode(BlendMode blendmode) { blendMode = blendmode; }
    public BlendMode getBlendMode() { return blendMode; }
    public int getMaxHeight() { return height; }
    public boolean draw(Canvas canvas) {
        boolean animating = releaseTime >= 0;
        getDistance();
        return animating;
    }
}
