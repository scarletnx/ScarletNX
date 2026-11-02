// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** An activity's action bar: activities here have none. */
public abstract class ActionBar {
    public abstract void show();
    public abstract void hide();
    public abstract boolean isShowing();
}
