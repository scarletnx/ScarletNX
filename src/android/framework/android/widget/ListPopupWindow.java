// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;

/** A list of the items of an adapter in a popup window next to an anchor view. The popup is not drawn (see PopupWindow). */
public class ListPopupWindow {
    public static final int POSITION_PROMPT_ABOVE = 0, POSITION_PROMPT_BELOW = 1;
    public static final int MATCH_PARENT = -1, WRAP_CONTENT = -2;
    public static final int INPUT_METHOD_FROM_FOCUSABLE = PopupWindow.INPUT_METHOD_FROM_FOCUSABLE,
        INPUT_METHOD_NEEDED = PopupWindow.INPUT_METHOD_NEEDED, INPUT_METHOD_NOT_NEEDED = PopupWindow.INPUT_METHOD_NOT_NEEDED;
    private final PopupWindow popup;
    private ListAdapter adapter;
    private View anchor, promptView;
    private int width = WRAP_CONTENT, height = WRAP_CONTENT, horizontalOffset, verticalOffset, promptPosition = POSITION_PROMPT_ABOVE, gravity, selection = -1;
    private boolean modal;
    private AdapterView.OnItemClickListener itemClickListener;
    private AdapterView.OnItemSelectedListener itemSelectedListener;

    public ListPopupWindow(Context context) { this(context, null); }
    public ListPopupWindow(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public ListPopupWindow(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public ListPopupWindow(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        popup = new PopupWindow(context, attrs, defStyleAttr, defStyleRes);
        popup.setContentView(new View(context));
    }

    public void setAdapter(ListAdapter adapter) { this.adapter = adapter; }
    public void setPromptPosition(int position) { promptPosition = position; }
    public int getPromptPosition() { return promptPosition; }
    public void setPromptView(View prompt) { promptView = prompt; }
    public void setModal(boolean modal) { this.modal = modal; popup.setFocusable(modal); }
    public boolean isModal() { return modal; }
    public void setSoftInputMode(int mode) { popup.setSoftInputMode(mode); }
    public int getSoftInputMode() { return popup.getSoftInputMode(); }
    public void setListSelector(Drawable selector) {}
    public Drawable getBackground() { return popup.getBackground(); }
    public void setBackgroundDrawable(Drawable d) { popup.setBackgroundDrawable(d); }
    public void setAnimationStyle(int animationStyle) { popup.setAnimationStyle(animationStyle); }
    public int getAnimationStyle() { return popup.getAnimationStyle(); }
    public View getAnchorView() { return anchor; }
    public void setAnchorView(View anchor) { this.anchor = anchor; }
    public int getHorizontalOffset() { return horizontalOffset; }
    public void setHorizontalOffset(int offset) { horizontalOffset = offset; }
    public int getVerticalOffset() { return verticalOffset; }
    public void setVerticalOffset(int offset) { verticalOffset = offset; }
    public void setDropDownGravity(int gravity) { this.gravity = gravity; }
    public int getWidth() { return width; }
    public void setWidth(int width) { this.width = width; }
    public void setContentWidth(int width) {
        Drawable bg = popup.getBackground();
        if (bg == null) { this.width = width; return; }
        android.graphics.Rect pad = new android.graphics.Rect();
        bg.getPadding(pad);
        this.width = pad.left + pad.right + width;
    }
    public int getHeight() { return height; }
    public void setHeight(int height) {
        if (height < 0 && height != WRAP_CONTENT && height != MATCH_PARENT) throw new IllegalArgumentException("Invalid height. Must be a positive value, MATCH_PARENT, or WRAP_CONTENT.");
        this.height = height;
    }
    public void setOnItemClickListener(AdapterView.OnItemClickListener l) { itemClickListener = l; }
    public void setOnItemSelectedListener(AdapterView.OnItemSelectedListener l) { itemSelectedListener = l; }
    public void show() { if (anchor != null) popup.showAsDropDown(anchor, horizontalOffset, verticalOffset, gravity); }
    public void dismiss() { popup.dismiss(); }
    public void setOnDismissListener(PopupWindow.OnDismissListener listener) { popup.setOnDismissListener(listener); }
    public void setInputMethodMode(int mode) { popup.setInputMethodMode(mode); }
    public int getInputMethodMode() { return popup.getInputMethodMode(); }
    public void setSelection(int position) { selection = position; }
    public void clearListSelection() { selection = -1; }
    public boolean isShowing() { return popup.isShowing(); }
    public boolean isInputMethodNotNeeded() { return popup.getInputMethodMode() == INPUT_METHOD_NOT_NEEDED; }
    /** A click on an item does what a tap on it in the list would do. */
    public boolean performItemClick(int position) {
        if (!isShowing() || itemClickListener == null || adapter == null) return false;
        itemClickListener.onItemClick(null, null, position, adapter.getItemId(position));
        return true;
    }
    public Object getSelectedItem() { return isShowing() && adapter != null && selection >= 0 ? adapter.getItem(selection) : null; }
    public int getSelectedItemPosition() { return isShowing() ? selection : -1; }
    public long getSelectedItemId() { return isShowing() && adapter != null && selection >= 0 ? adapter.getItemId(selection) : Long.MIN_VALUE; }
    public View getSelectedView() { return null; }
    public ListView getListView() { return null; }
    /** A drag from the source view does not open the list: the listener takes no touch. */
    public View.OnTouchListener createDragToOpenListener(View src) { return (v, event) -> false; }
}
