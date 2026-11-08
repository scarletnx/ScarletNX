// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.text.Editable;
import android.text.Selection;
import android.text.TextWatcher;
import android.util.AttributeSet;
import android.view.View;

/**
 * An editable text view that filters an adapter with what the user types. The runtime has no drop-down window:
 * the list of completions is not drawn, and the state of the drop-down is kept for the app.
 */
public class AutoCompleteTextView extends EditText implements Filter.FilterListener {
    private ListAdapter adapter;
    private Filter filter;
    private int threshold = 2, dropDownWidth = -2, dropDownHeight = -2, dropDownHorizontalOffset, dropDownVerticalOffset, listSelection = ListView.INVALID_POSITION;
    private boolean popupShowing, blockCompletion;
    private CharSequence completionHint;
    private Drawable dropDownBackground;
    private AdapterView.OnItemClickListener itemClickListener;
    private AdapterView.OnItemSelectedListener itemSelectedListener;
    private OnDismissListener dismissListener;
    private Validator validator;

    public interface Validator { boolean isValid(CharSequence text); CharSequence fixText(CharSequence invalidText); }
    public interface OnDismissListener { void onDismiss(); }

    public AutoCompleteTextView(Context context) { this(context, null); }
    public AutoCompleteTextView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public AutoCompleteTextView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public AutoCompleteTextView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { this(context, attrs, defStyleAttr, defStyleRes, null); }
    public AutoCompleteTextView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes, Resources.Theme popupTheme) {
        super(context, attrs, defStyleAttr, defStyleRes);
        addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) { doAfterTextChanged(); }
        });
    }

    private void doAfterTextChanged() {
        if (blockCompletion) return;
        if (enoughToFilter()) { if (filter != null) performFiltering(getText(), 0); }
        else dismissDropDown();
    }

    public <T extends ListAdapter & Filterable> void setAdapter(T adapter) {
        this.adapter = adapter;
        filter = adapter != null ? adapter.getFilter() : null;
    }
    public ListAdapter getAdapter() { return adapter; }
    protected Filter getFilter() { return filter; }
    public void setThreshold(int threshold) { this.threshold = threshold <= 0 ? 1 : threshold; }
    public int getThreshold() { return threshold; }
    public boolean enoughToFilter() { return getText().length() >= threshold; }
    protected void performFiltering(CharSequence text, int keyCode) { filter.filter(text, this); }
    @Override public void onFilterComplete(int count) {
        if (count > 0 && enoughToFilter() && hasFocus()) showDropDown(); else dismissDropDown();
    }
    public void showDropDown() { popupShowing = true; }
    public void dismissDropDown() {
        if (!popupShowing) return;
        popupShowing = false;
        if (dismissListener != null) dismissListener.onDismiss();
    }
    public boolean isPopupShowing() { return popupShowing; }
    public void setOnItemClickListener(AdapterView.OnItemClickListener l) { itemClickListener = l; }
    public AdapterView.OnItemClickListener getOnItemClickListener() { return itemClickListener; }
    public void setOnItemSelectedListener(AdapterView.OnItemSelectedListener l) { itemSelectedListener = l; }
    public AdapterView.OnItemSelectedListener getOnItemSelectedListener() { return itemSelectedListener; }
    public void setOnDismissListener(OnDismissListener dismissListener) { this.dismissListener = dismissListener; }
    public void setCompletionHint(CharSequence hint) { completionHint = hint; }
    public CharSequence getCompletionHint() { return completionHint; }
    public int getDropDownWidth() { return dropDownWidth; }
    public void setDropDownWidth(int width) { dropDownWidth = width; }
    public int getDropDownHeight() { return dropDownHeight; }
    public void setDropDownHeight(int height) { dropDownHeight = height; }
    public void setDropDownHorizontalOffset(int offset) { dropDownHorizontalOffset = offset; }
    public int getDropDownHorizontalOffset() { return dropDownHorizontalOffset; }
    public void setDropDownVerticalOffset(int offset) { dropDownVerticalOffset = offset; }
    public int getDropDownVerticalOffset() { return dropDownVerticalOffset; }
    public void setDropDownBackgroundDrawable(Drawable d) { dropDownBackground = d; }
    public void setDropDownBackgroundResource(int id) { dropDownBackground = getContext().getDrawable(id); }
    public Drawable getDropDownBackground() { return dropDownBackground; }
    public void setDropDownAnchor(int id) {}
    public int getListSelection() { return popupShowing ? listSelection : ListView.INVALID_POSITION; }
    public void setListSelection(int position) { if (popupShowing && position >= 0) listSelection = position; }
    public void clearListSelection() { listSelection = ListView.INVALID_POSITION; }
    /** Puts the item at the list selection into the text, as a click on it in the drop-down would. */
    public void performCompletion() {
        if (adapter == null || listSelection < 0 || listSelection >= adapter.getCount()) return;
        Object item = adapter.getItem(listSelection);
        blockCompletion = true;
        replaceText(convertSelectionToString(item));
        blockCompletion = false;
        if (itemClickListener != null) itemClickListener.onItemClick(null, (View) null, listSelection, adapter.getItemId(listSelection));
        dismissDropDown();
    }
    public boolean isPerformingCompletion() { return blockCompletion; }
    protected CharSequence convertSelectionToString(Object selectedItem) { return filter != null ? filter.convertResultToString(selectedItem) : String.valueOf(selectedItem); }
    protected void replaceText(CharSequence text) {
        clearComposingText();
        setText(text);
        Editable e = getText();
        Selection.setSelection(e, e.length());
    }
    public void clearComposingText() {}
    public void setText(CharSequence text, boolean filter) {
        if (filter) { setText(text); return; }
        blockCompletion = true;
        setText(text);
        blockCompletion = false;
    }
    public void setValidator(Validator validator) { this.validator = validator; }
    public Validator getValidator() { return validator; }
    public void performValidation() {
        if (validator == null) return;
        CharSequence text = getText();
        if (!android.text.TextUtils.isEmpty(text) && !validator.isValid(text)) setText(validator.fixText(text));
    }
    @Override public CharSequence getAccessibilityClassName() { return AutoCompleteTextView.class.getName(); }
}
