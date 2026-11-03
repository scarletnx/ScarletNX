// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** An item of a menu. */
public interface MenuItem {
    int SHOW_AS_ACTION_NEVER = 0, SHOW_AS_ACTION_IF_ROOM = 1, SHOW_AS_ACTION_ALWAYS = 2, SHOW_AS_ACTION_WITH_TEXT = 4,
        SHOW_AS_ACTION_COLLAPSE_ACTION_VIEW = 8;

    interface OnMenuItemClickListener { boolean onMenuItemClick(MenuItem item); }

    interface OnActionExpandListener {
        boolean onMenuItemActionExpand(MenuItem item);
        boolean onMenuItemActionCollapse(MenuItem item);
    }

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
    MenuItem setTitleCondensed(CharSequence title);
    CharSequence getTitleCondensed();
    MenuItem setIntent(android.content.Intent intent);
    android.content.Intent getIntent();
    MenuItem setShortcut(char numericChar, char alphaChar);
    MenuItem setNumericShortcut(char numericChar);
    char getNumericShortcut();
    MenuItem setAlphabeticShortcut(char alphaChar);
    char getAlphabeticShortcut();
    boolean hasSubMenu();
    SubMenu getSubMenu();
    ContextMenu.ContextMenuInfo getMenuInfo();
    MenuItem setActionView(View view);
    MenuItem setActionView(int resId);
    View getActionView();
    MenuItem setActionProvider(ActionProvider actionProvider);
    ActionProvider getActionProvider();
    boolean expandActionView();
    boolean collapseActionView();
    boolean isActionViewExpanded();
    MenuItem setOnActionExpandListener(OnActionExpandListener listener);
    default MenuItem setShortcut(char numericChar, char alphaChar, int numericModifiers, int alphaModifiers) {
        if ((alphaModifiers & Menu.SUPPORTED_MODIFIERS_MASK) == KeyEvent.META_CTRL_ON
                && (numericModifiers & Menu.SUPPORTED_MODIFIERS_MASK) == KeyEvent.META_CTRL_ON) return setShortcut(numericChar, alphaChar);
        return this;
    }
    default MenuItem setNumericShortcut(char numericChar, int numericModifiers) {
        return (numericModifiers & Menu.SUPPORTED_MODIFIERS_MASK) == KeyEvent.META_CTRL_ON ? setNumericShortcut(numericChar) : this;
    }
    default int getNumericModifiers() { return KeyEvent.META_CTRL_ON; }
    default MenuItem setAlphabeticShortcut(char alphaChar, int alphaModifiers) {
        return (alphaModifiers & Menu.SUPPORTED_MODIFIERS_MASK) == KeyEvent.META_CTRL_ON ? setAlphabeticShortcut(alphaChar) : this;
    }
    default int getAlphabeticModifiers() { return KeyEvent.META_CTRL_ON; }
    default MenuItem setContentDescription(CharSequence contentDescription) { return this; }
    default CharSequence getContentDescription() { return null; }
    default MenuItem setTooltipText(CharSequence tooltipText) { return this; }
    default CharSequence getTooltipText() { return null; }
    default MenuItem setIconTintList(android.content.res.ColorStateList tint) { return this; }
    default android.content.res.ColorStateList getIconTintList() { return null; }
    default MenuItem setIconTintMode(android.graphics.PorterDuff.Mode tintMode) { return this; }
    default android.graphics.PorterDuff.Mode getIconTintMode() { return null; }
}
