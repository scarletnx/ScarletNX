// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** An item of a menu. */
public interface MenuItem {
    int SHOW_AS_ACTION_NEVER = 0, SHOW_AS_ACTION_IF_ROOM = 1, SHOW_AS_ACTION_ALWAYS = 2, SHOW_AS_ACTION_WITH_TEXT = 4,
        SHOW_AS_ACTION_COLLAPSE_ACTION_VIEW = 8;

    interface OnMenuItemClickListener { boolean onMenuItemClick(MenuItem item); }

    int getItemId();
    int getGroupId();
    int getOrder();
    MenuItem setTitle(CharSequence title);
    MenuItem setTitle(int title);
    CharSequence getTitle();
    MenuItem setIcon(android.graphics.drawable.Drawable icon);
    MenuItem setIcon(int iconRes);
    android.graphics.drawable.Drawable getIcon();
    MenuItem setVisible(boolean visible);
    boolean isVisible();
    MenuItem setEnabled(boolean enabled);
    boolean isEnabled();
    MenuItem setOnMenuItemClickListener(OnMenuItemClickListener menuItemClickListener);
    void setShowAsAction(int actionEnum);
    MenuItem setShowAsActionFlags(int actionEnum);
    MenuItem setCheckable(boolean checkable);
    boolean isCheckable();
    MenuItem setChecked(boolean checked);
    boolean isChecked();
}
