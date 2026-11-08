// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.transition;

/** Moves and resizes views to their new layout; here they take it at once. */
public class ChangeBounds extends Transition {
    private boolean resizeClip, reparent;

    public ChangeBounds() {}
    public ChangeBounds(android.content.Context context, android.util.AttributeSet attrs) { super(context, attrs); }
    public void setResizeClip(boolean resizeClip) { this.resizeClip = resizeClip; }
    public boolean getResizeClip() { return resizeClip; }
    @Deprecated public void setReparent(boolean reparent) { this.reparent = reparent; }
    @Override public String[] getTransitionProperties() {
        return new String[] {"android:changeBounds:bounds", "android:changeBounds:clip", "android:changeBounds:parent",
            "android:changeBounds:windowX", "android:changeBounds:windowY"};
    }
}
