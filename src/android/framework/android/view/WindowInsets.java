// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Insets;
import android.graphics.Rect;

/** A window's insets: none here, as the window has no system bars, cutout or keyboard over it. */
public final class WindowInsets {
    public static final WindowInsets CONSUMED = new WindowInsets(Insets.NONE, true);
    private final Insets system;
    private final boolean consumed;

    private WindowInsets(Insets system, boolean consumed) { this.system = system; this.consumed = consumed; }
    public WindowInsets(WindowInsets src) { this(src.system, src.consumed); }
    /** The insets a window gets: none, not consumed. */
    public static WindowInsets none() { return new WindowInsets(Insets.NONE, false); }

    public static final class Type {
        public static int statusBars() { return 1; }
        public static int navigationBars() { return 2; }
        public static int captionBar() { return 4; }
        public static int ime() { return 8; }
        public static int systemGestures() { return 16; }
        public static int mandatorySystemGestures() { return 32; }
        public static int tappableElement() { return 64; }
        public static int displayCutout() { return 128; }
        public static int systemOverlays() { return 256; }
        public static int systemBars() { return statusBars() | navigationBars() | captionBar(); }
    }

    public static final class Side {
        public static final int LEFT = 1, TOP = 2, RIGHT = 4, BOTTOM = 8;
        public static int all() { return 15; }
    }

    public static final class Builder {
        private Insets insets = Insets.NONE;
        public Builder() {}
        public Builder(WindowInsets insets) { this.insets = insets.system; }
        public Builder setSystemWindowInsets(Insets i) { insets = i; return this; }
        public Builder setSystemGestureInsets(Insets i) { return this; }
        public Builder setMandatorySystemGestureInsets(Insets i) { return this; }
        public Builder setTappableElementInsets(Insets i) { return this; }
        public Builder setStableInsets(Insets i) { return this; }
        public Builder setDisplayCutout(DisplayCutout c) { return this; }
        public Builder setInsets(int typeMask, Insets i) { insets = i; return this; }
        public Builder setInsetsIgnoringVisibility(int typeMask, Insets i) { return this; }
        public Builder setVisible(int typeMask, boolean visible) { return this; }
        public Builder setRoundedCorner(int position, RoundedCorner roundedCorner) { return this; }
        public Builder setPrivacyIndicatorBounds(Rect bounds) { return this; }
        public WindowInsets build() { return new WindowInsets(insets, false); }
    }

    public Insets getInsets(int typeMask) { return system; }
    public Insets getInsetsIgnoringVisibility(int typeMask) { return system; }
    public boolean isVisible(int typeMask) { return false; }
    public Insets getSystemWindowInsets() { return system; }
    public int getSystemWindowInsetLeft() { return system.left; }
    public int getSystemWindowInsetTop() { return system.top; }
    public int getSystemWindowInsetRight() { return system.right; }
    public int getSystemWindowInsetBottom() { return system.bottom; }
    public boolean hasSystemWindowInsets() { return !system.equals(Insets.NONE); }
    public boolean hasInsets() { return hasSystemWindowInsets(); }
    public Insets getStableInsets() { return Insets.NONE; }
    public int getStableInsetLeft() { return 0; }
    public int getStableInsetTop() { return 0; }
    public int getStableInsetRight() { return 0; }
    public int getStableInsetBottom() { return 0; }
    public boolean hasStableInsets() { return false; }
    public Insets getSystemGestureInsets() { return Insets.NONE; }
    public Insets getMandatorySystemGestureInsets() { return Insets.NONE; }
    public Insets getTappableElementInsets() { return Insets.NONE; }
    public DisplayCutout getDisplayCutout() { return null; }
    public RoundedCorner getRoundedCorner(int position) { return null; }
    public Rect getPrivacyIndicatorBounds() { return null; }
    public boolean isConsumed() { return consumed; }
    public boolean isRound() { return false; }
    public WindowInsets consumeSystemWindowInsets() { return new WindowInsets(Insets.NONE, true); }
    public WindowInsets consumeStableInsets() { return this; }
    public WindowInsets consumeDisplayCutout() { return this; }
    public WindowInsets replaceSystemWindowInsets(int left, int top, int right, int bottom) { return new WindowInsets(Insets.of(left, top, right, bottom), consumed); }
    public WindowInsets replaceSystemWindowInsets(Rect r) { return replaceSystemWindowInsets(r.left, r.top, r.right, r.bottom); }
    public WindowInsets inset(Insets insets) { return this; }
    public WindowInsets inset(int left, int top, int right, int bottom) { return this; }
    @Override public boolean equals(Object o) { return o instanceof WindowInsets w && w.system.equals(system) && w.consumed == consumed; }
    @Override public int hashCode() { return system.hashCode() * 2 + (consumed ? 1 : 0); }
    @Override public String toString() { return "WindowInsets{" + system + (consumed ? " consumed" : "") + "}"; }
}
