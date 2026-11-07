// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.transition.Transition;
import android.util.AttributeSet;
import android.view.View;

/**
 * A window over the app, next to an anchor view or at a place on the screen. The runtime has one window for each
 * activity and no window of its own for a popup: a popup keeps its state and its content view, and it is not drawn.
 */
public class PopupWindow {
    public static final int INPUT_METHOD_FROM_FOCUSABLE = 0, INPUT_METHOD_NEEDED = 1, INPUT_METHOD_NOT_NEEDED = 2;
    private View contentView;
    private int width, height, animationStyle = -1, inputMethodMode = INPUT_METHOD_FROM_FOCUSABLE, softInputMode, windowLayoutType = 1000;
    private boolean showing, focusable, touchable = true, outsideTouchable, clippingEnabled = true, overlapAnchor, aboveAnchor, touchModal = true, clippedToScreen;
    private float elevation;
    private Drawable background;
    private Transition enterTransition, exitTransition;
    private Rect epicenterBounds;
    private OnDismissListener onDismissListener;
    private View.OnTouchListener touchInterceptor;

    public interface OnDismissListener { void onDismiss(); }

    public PopupWindow(Context context) { this(context, null); }
    public PopupWindow(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public PopupWindow(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public PopupWindow(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {}
    public PopupWindow() { this(null, 0, 0); }
    public PopupWindow(View contentView) { this(contentView, 0, 0); }
    public PopupWindow(int width, int height) { this(null, width, height); }
    public PopupWindow(View contentView, int width, int height) { this(contentView, width, height, false); }
    public PopupWindow(View contentView, int width, int height, boolean focusable) {
        this.contentView = contentView;
        this.width = width;
        this.height = height;
        this.focusable = focusable;
    }

    public void setEnterTransition(Transition enterTransition) { this.enterTransition = enterTransition; }
    public Transition getEnterTransition() { return enterTransition; }
    public void setExitTransition(Transition exitTransition) { this.exitTransition = exitTransition; }
    public Transition getExitTransition() { return exitTransition; }
    public void setEpicenterBounds(Rect bounds) { epicenterBounds = bounds != null ? new Rect(bounds) : null; }
    public Rect getEpicenterBounds() { return epicenterBounds != null ? new Rect(epicenterBounds) : null; }
    public Drawable getBackground() { return background; }
    public void setBackgroundDrawable(Drawable background) { this.background = background; }
    public float getElevation() { return elevation; }
    public void setElevation(float elevation) { this.elevation = elevation; }
    public int getAnimationStyle() { return animationStyle; }
    public void setAnimationStyle(int animationStyle) { this.animationStyle = animationStyle; }
    public View getContentView() { return contentView; }
    public void setContentView(View contentView) { if (!showing) this.contentView = contentView; }
    public void setTouchInterceptor(View.OnTouchListener l) { touchInterceptor = l; }
    public boolean isFocusable() { return focusable; }
    public void setFocusable(boolean focusable) { this.focusable = focusable; }
    public int getInputMethodMode() { return inputMethodMode; }
    public void setInputMethodMode(int mode) { inputMethodMode = mode; }
    public void setSoftInputMode(int mode) { softInputMode = mode; }
    public int getSoftInputMode() { return softInputMode; }
    public boolean isTouchable() { return touchable; }
    public void setTouchable(boolean touchable) { this.touchable = touchable; }
    public boolean isOutsideTouchable() { return outsideTouchable; }
    public void setOutsideTouchable(boolean touchable) { outsideTouchable = touchable; }
    public boolean isClippingEnabled() { return clippingEnabled; }
    public void setClippingEnabled(boolean enabled) { clippingEnabled = enabled; }
    public void setIsClippedToScreen(boolean enabled) { clippedToScreen = enabled; }
    public boolean isClippedToScreen() { return clippedToScreen; }
    public void setOverlapAnchor(boolean overlapAnchor) { this.overlapAnchor = overlapAnchor; }
    public boolean getOverlapAnchor() { return overlapAnchor; }
    public void setTouchModal(boolean touchModal) { this.touchModal = touchModal; }
    public boolean isTouchModal() { return touchModal; }
    public void setWindowLayoutType(int layoutType) { windowLayoutType = layoutType; }
    public int getWindowLayoutType() { return windowLayoutType; }
    public int getHeight() { return height; }
    public void setHeight(int height) { this.height = height; }
    public int getWidth() { return width; }
    public void setWidth(int width) { this.width = width; }
    public boolean isAboveAnchor() { return aboveAnchor; }
    public boolean isShowing() { return showing; }

    public void showAtLocation(View parent, int gravity, int x, int y) { show(); aboveAnchor = false; }
    public void showAsDropDown(View anchor) { showAsDropDown(anchor, 0, 0); }
    public void showAsDropDown(View anchor, int xoff, int yoff) { showAsDropDown(anchor, xoff, yoff, android.view.Gravity.TOP | android.view.Gravity.START); }
    /** The popup goes above the anchor when the space below it is smaller than the popup and the space above. */
    public void showAsDropDown(View anchor, int xoff, int yoff, int gravity) {
        if (showing || contentView == null) return;
        Rect frame = new Rect();
        anchor.getWindowVisibleDisplayFrame(frame);
        int[] at = new int[2];
        anchor.getLocationOnScreen(at);
        int below = frame.bottom - (at[1] + anchor.getHeight()) - yoff, above = at[1] - frame.top + yoff;
        int h = height > 0 ? height : contentView.getMeasuredHeight();
        aboveAnchor = h > below && above > below;
        show();
    }
    private void show() { if (contentView != null) showing = true; }
    public int getMaxAvailableHeight(View anchor) { return getMaxAvailableHeight(anchor, 0); }
    public int getMaxAvailableHeight(View anchor, int yOffset) { return getMaxAvailableHeight(anchor, yOffset, false); }
    public int getMaxAvailableHeight(View anchor, int yOffset, boolean ignoreBottomDecorations) {
        Rect frame = new Rect();
        anchor.getWindowVisibleDisplayFrame(frame);
        int[] at = new int[2];
        anchor.getLocationOnScreen(at);
        int below = frame.bottom - (at[1] + anchor.getHeight()) - yOffset, above = at[1] - frame.top + yOffset;
        int space = Math.max(below, above);
        if (background != null) { Rect pad = new Rect(); background.getPadding(pad); space -= pad.top + pad.bottom; }
        return space;
    }
    public void dismiss() {
        if (!showing) return;
        showing = false;
        if (onDismissListener != null) onDismissListener.onDismiss();
    }
    public void setOnDismissListener(OnDismissListener onDismissListener) { this.onDismissListener = onDismissListener; }
    public void update() {}
    public void update(int width, int height) { update(-1, -1, width, height); }
    public void update(int x, int y, int width, int height) { update(x, y, width, height, false); }
    public void update(int x, int y, int width, int height, boolean force) { if (width >= 0) this.width = width; if (height >= 0) this.height = height; }
    public void update(View anchor, int width, int height) { update(anchor, 0, 0, width, height); }
    public void update(View anchor, int xoff, int yoff, int width, int height) { update(xoff, yoff, width, height); }
}
