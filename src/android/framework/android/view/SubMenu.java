// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A menu inside an item of another menu. */
public interface SubMenu extends Menu {
    SubMenu setHeaderTitle(int titleRes);
    SubMenu setHeaderTitle(CharSequence title);
    SubMenu setHeaderIcon(int iconRes);
    SubMenu setHeaderIcon(android.graphics.drawable.Drawable icon);
    SubMenu setHeaderView(View view);
    void clearHeader();
    SubMenu setIcon(int iconRes);
    SubMenu setIcon(android.graphics.drawable.Drawable icon);
    MenuItem getItem();
}
