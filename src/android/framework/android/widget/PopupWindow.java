// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.os.IBinder;
import android.transition.Transition;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;

/**
 * A window over the app, next to an anchor view or at a place on the screen: a window of its own, as Android's, added
 * to the window manager on the anchor's window token. Its content goes in a frame that draws the background and
 * dismisses the popup on the back key and on a touch outside it.
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
    private PopupDecorView decor; // the window's root while it shows
    private WindowManager windowManager;

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

    /** At (x, y) from the gravity's edges of the display, Gravity.NO_GRAVITY meaning the top left */
    public void showAtLocation(View parent, int gravity, int x, int y) {
        showAtLocation(parent.getWindowToken(), gravity, x, y, parent.getContext());
    }
    private void showAtLocation(IBinder token, int gravity, int x, int y, Context context) {
        if (showing || contentView == null) return;
        aboveAnchor = false;
        WindowManager.LayoutParams p = params(token);
        p.gravity = gravity == Gravity.NO_GRAVITY ? Gravity.TOP | Gravity.START : gravity;
        p.x = x;
        p.y = y;
        attach(p, context);
    }
    public void showAsDropDown(View anchor) { showAsDropDown(anchor, 0, 0); }
    public void showAsDropDown(View anchor, int xoff, int yoff) {
        showAsDropDown(anchor, xoff, yoff, Gravity.TOP | Gravity.START);
    }
    /**
     * Below the anchor's left edge (its right one for an end gravity), moved by the offsets; above it when the space
     * below is smaller than the popup and than the space above.
     */
    public void showAsDropDown(View anchor, int xoff, int yoff, int gravity) {
        if (showing || contentView == null) return;
        WindowManager.LayoutParams p = params(anchor.getWindowToken());
        p.gravity = Gravity.TOP | Gravity.START;
        place(anchor, xoff, yoff, gravity, p);
        attach(p, anchor.getContext());
    }
    /** p's x and y below or above the anchor, and aboveAnchor */
    private void place(View anchor, int xoff, int yoff, int gravity, WindowManager.LayoutParams p) {
        Rect frame = new Rect();
        anchor.getWindowVisibleDisplayFrame(frame);
        int[] at = new int[2];
        anchor.getLocationOnScreen(at);
        int w = p.width > 0 ? p.width : measured(true), h = p.height > 0 ? p.height : measured(false);
        int below = frame.bottom - (at[1] + anchor.getHeight()) - yoff, above = at[1] - frame.top + yoff;
        aboveAnchor = h > below && above > below;
        int horizontal =
            Gravity.getAbsoluteGravity(gravity, anchor.getLayoutDirection()) & Gravity.HORIZONTAL_GRAVITY_MASK;
        p.x = at[0] + xoff + (horizontal == Gravity.RIGHT ? anchor.getWidth() - w : 0);
        p.y = aboveAnchor ? at[1] - h - yoff : at[1] + anchor.getHeight() + yoff;
    }
    /** the content's size as it wraps, in the display */
    private int measured(boolean width) {
        android.util.DisplayMetrics m = snx.android.Device.metrics();
        int specW = View.MeasureSpec.makeMeasureSpec(m.widthPixels, View.MeasureSpec.AT_MOST);
        int specH = View.MeasureSpec.makeMeasureSpec(m.heightPixels, View.MeasureSpec.AT_MOST);
        View v = decor != null ? decor : contentView;
        v.measure(specW, specH);
        return width ? v.getMeasuredWidth() : v.getMeasuredHeight();
    }
    /** the window's params, as Android's PopupWindow makes them from its state */
    private WindowManager.LayoutParams params(IBinder token) {
        WindowManager.LayoutParams p = new WindowManager.LayoutParams();
        p.width = width;
        p.height = height;
        p.type = windowLayoutType;
        p.token = token;
        p.softInputMode = softInputMode;
        p.format = android.graphics.PixelFormat.TRANSLUCENT;
        p.flags = flags();
        return p;
    }
    private int flags() {
        int f = 0;
        if (!focusable) f |= WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;
        if (!touchable) f |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        if (outsideTouchable) f |= WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH;
        if (!clippingEnabled || clippedToScreen) f |= WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS;
        if (!touchModal) f |= WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL;
        return f;
    }
    /** the content in its frame, added as a window: Android's message for a token of no window */
    private void attach(WindowManager.LayoutParams p, Context context) {
        if (p.token == null)
            throw new WindowManager.BadTokenException(
                "Unable to add window -- token null is not valid; is your activity running?");
        Context c = context != null ? context : contentView.getContext();
        decor = new PopupDecorView(c);
        if (contentView.getParent() instanceof ViewGroup old) old.removeView(contentView);
        decor.addView(contentView,
            new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        decor.setBackground(background);
        decor.setElevation(elevation);
        windowManager = (WindowManager) c.getSystemService(Context.WINDOW_SERVICE);
        windowManager.addView(decor, p);
        showing = true;
    }
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
        PopupDecorView d = decor;
        decor = null;
        windowManager.removeViewImmediate(d);
        d.removeView(contentView);
        if (onDismissListener != null) onDismissListener.onDismiss();
    }
    public void setOnDismissListener(OnDismissListener onDismissListener) { this.onDismissListener = onDismissListener; }
    /** the shown window's params again from the popup's state: its flags, and its size and place as given */
    public void update() {
        if (!showing) return;
        WindowManager.LayoutParams p = (WindowManager.LayoutParams) decor.getLayoutParams();
        p.flags = flags();
        p.softInputMode = softInputMode;
        windowManager.updateViewLayout(decor, p);
    }
    public void update(int width, int height) {
        if (!showing) { setWidth(width); setHeight(height); return; }
        WindowManager.LayoutParams p = (WindowManager.LayoutParams) decor.getLayoutParams();
        update(p.x, p.y, width, height, false);
    }
    public void update(int x, int y, int width, int height) { update(x, y, width, height, false); }
    /** a size of -1 keeps the window's, a place of -1 too (both or neither) */
    public void update(int x, int y, int width, int height, boolean force) {
        if (width >= 0) this.width = width;
        if (height >= 0) this.height = height;
        if (!showing) return;
        WindowManager.LayoutParams p = (WindowManager.LayoutParams) decor.getLayoutParams();
        if (width >= 0) p.width = width;
        if (height >= 0) p.height = height;
        if (x != -1 || y != -1) { p.x = x; p.y = y; }
        p.flags = flags();
        windowManager.updateViewLayout(decor, p);
    }
    public void update(View anchor, int width, int height) { update(anchor, 0, 0, width, height); }
    public void update(View anchor, int xoff, int yoff, int width, int height) {
        if (width >= 0) this.width = width;
        if (height >= 0) this.height = height;
        if (!showing) return;
        WindowManager.LayoutParams p = (WindowManager.LayoutParams) decor.getLayoutParams();
        if (width >= 0) p.width = width;
        if (height >= 0) p.height = height;
        place(anchor, xoff, yoff, Gravity.TOP | Gravity.START, p);
        windowManager.updateViewLayout(decor, p);
    }

    /** The window's root: the content's frame, with the popup's background, and its dismissals as Android's */
    private final class PopupDecorView extends android.widget.FrameLayout {
        PopupDecorView(Context context) { super(context); }
        @Override public boolean dispatchKeyEvent(KeyEvent event) {
            if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
                if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) dismiss();
                return true;
            }
            return super.dispatchKeyEvent(event);
        }
        @Override public boolean dispatchTouchEvent(MotionEvent event) {
            if (touchInterceptor != null && touchInterceptor.onTouch(this, event)) return true;
            return super.dispatchTouchEvent(event);
        }
        /** a touch outside: a down beyond its edges (a touch-modal popup gets those) or ACTION_OUTSIDE */
        @Override public boolean onTouchEvent(MotionEvent event) {
            int x = (int) event.getX(), y = (int) event.getY();
            boolean outsideDown = event.getAction() == MotionEvent.ACTION_DOWN
                && (x < 0 || x >= getWidth() || y < 0 || y >= getHeight());
            if (outsideDown || event.getAction() == MotionEvent.ACTION_OUTSIDE) {
                dismiss();
                return true;
            }
            return super.onTouchEvent(event);
        }
    }
}
