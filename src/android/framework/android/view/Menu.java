// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A menu of items. */
public interface Menu {
    int NONE = 0, FIRST = 1, CATEGORY_CONTAINER = 0x10000, CATEGORY_SYSTEM = 0x20000, CATEGORY_SECONDARY = 0x30000,
        CATEGORY_ALTERNATIVE = 0x40000, FLAG_APPEND_TO_GROUP = 1, FLAG_PERFORM_NO_CLOSE = 1, FLAG_ALWAYS_PERFORM_CLOSE = 2,
        SUPPORTED_MODIFIERS_MASK = 0x1100f;
    MenuItem add(CharSequence title);
    MenuItem add(int titleRes);
    MenuItem add(int groupId, int itemId, int order, CharSequence title);
    MenuItem add(int groupId, int itemId, int order, int titleRes);
    void removeItem(int id);
    void removeGroup(int groupId);
    void clear();
    void setGroupVisible(int group, boolean visible);
    void setGroupEnabled(int group, boolean enabled);
    boolean hasVisibleItems();
    MenuItem findItem(int id);
    int size();
    MenuItem getItem(int index);
    void close();
    boolean performShortcut(int keyCode, KeyEvent event, int flags);
    boolean isShortcutKey(int keyCode, KeyEvent event);
    boolean performIdentifierAction(int id, int flags);
    void setQwertyMode(boolean isQwerty);
    SubMenu addSubMenu(CharSequence title);
    SubMenu addSubMenu(int titleRes);
    SubMenu addSubMenu(int groupId, int itemId, int order, CharSequence title);
    SubMenu addSubMenu(int groupId, int itemId, int order, int titleRes);
    void setGroupCheckable(int group, boolean checkable, boolean exclusive);
    default void setGroupDividerEnabled(boolean groupDividerEnabled) {}
}
