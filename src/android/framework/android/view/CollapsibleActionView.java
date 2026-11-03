// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** An action view that hears when its menu item expands or collapses it. */
public interface CollapsibleActionView {
    void onActionViewExpanded();
    void onActionViewCollapsed();
}
