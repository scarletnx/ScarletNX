// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.view.MenuItem;
import android.view.View;

/** A menu in a popup next to an anchor view. The popup is not drawn (see PopupWindow). */
public class PopupMenu {
    private final View anchor;
    private int gravity;
    private boolean showing;
    private OnMenuItemClickListener menuItemClickListener;
    private OnDismissListener onDismissListener;

    public interface OnMenuItemClickListener { boolean onMenuItemClick(MenuItem item); }
    public interface OnDismissListener { void onDismiss(PopupMenu menu); }

    public PopupMenu(Context context, View anchor) { this(context, anchor, android.view.Gravity.NO_GRAVITY); }
    public PopupMenu(Context context, View anchor, int gravity) { this(context, anchor, gravity, 0, 0); }
    public PopupMenu(Context context, View anchor, int gravity, int popupStyleAttr, int popupStyleRes) { this.anchor = anchor; this.gravity = gravity; }

    public void setGravity(int gravity) { this.gravity = gravity; }
    public int getGravity() { return gravity; }
    /** A drag from the anchor does not open the menu: the listener takes no touch. */
    public View.OnTouchListener getDragToOpenListener() { return (v, event) -> false; }
    public void show() { showing = anchor != null; }
    public void dismiss() {
        if (!showing) return;
        showing = false;
        if (onDismissListener != null) onDismissListener.onDismiss(this);
    }
    public void setOnMenuItemClickListener(OnMenuItemClickListener listener) { menuItemClickListener = listener; }
    public void setOnDismissListener(OnDismissListener listener) { onDismissListener = listener; }
    public void setForceShowIcon(boolean forceShowIcon) {}
}
