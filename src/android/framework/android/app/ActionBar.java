// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** An activity's action bar: activities here have none. */
public abstract class ActionBar {
    public abstract void show();
    public abstract void hide();
    public abstract boolean isShowing();
    public abstract int getDisplayOptions();
    public android.content.Context getThemedContext() { return null; }
    public void setHomeActionContentDescription(int resId) {}
    public void setHomeAsUpIndicator(android.graphics.drawable.Drawable indicator) {}
}
