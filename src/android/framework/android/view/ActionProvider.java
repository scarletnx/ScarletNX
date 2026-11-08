// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.Context;

/** An action view and a submenu of a menu item, made by the app. */
public abstract class ActionProvider {
    private VisibilityListener visibilityListener;

    public interface VisibilityListener {
        void onActionProviderVisibilityChanged(boolean isVisible);
    }

    public ActionProvider(Context context) {}
    @Deprecated public abstract View onCreateActionView();
    public View onCreateActionView(MenuItem forItem) { return onCreateActionView(); }
    public boolean overridesItemVisibility() { return false; }
    public boolean isVisible() { return true; }
    public void refreshVisibility() {
        if (visibilityListener != null && overridesItemVisibility()) visibilityListener.onActionProviderVisibilityChanged(isVisible());
    }
    public boolean onPerformDefaultAction() { return false; }
    public boolean hasSubMenu() { return false; }
    public void onPrepareSubMenu(SubMenu subMenu) {}
    public void setVisibilityListener(VisibilityListener listener) { visibilityListener = listener; }
}
