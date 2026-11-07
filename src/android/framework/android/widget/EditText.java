// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.text.Editable;
import android.text.InputType;
import android.text.Selection;
import android.util.AttributeSet;

/** A text view whose text the user edits. */
public class EditText extends TextView {
    public EditText(Context context) { this(context, null); }
    public EditText(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public EditText(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public EditText(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setClickable(true);
        setLongClickable(true);
        setRawInputType(InputType.TYPE_CLASS_TEXT);
        setText("");
    }

    public boolean getFreezesText() { return true; }
    @Override protected boolean getDefaultEditable() { return true; }
    @Override public Editable getText() {
        CharSequence text = super.getText();
        if (text instanceof Editable e) return e;
        super.setText(text, BufferType.EDITABLE);
        return (Editable) super.getText();
    }
    @Override public void setText(CharSequence text, BufferType type) { super.setText(text, BufferType.EDITABLE); }
    public void setSelection(int start, int stop) { Selection.setSelection(getText(), start, stop); }
    public void setSelection(int index) { Selection.setSelection(getText(), index); }
    public void selectAll() { Editable e = getText(); Selection.setSelection(e, 0, e.length()); }
    public void extendSelection(int index) { Editable e = getText(); int start = Selection.getSelectionStart(e); Selection.setSelection(e, start < 0 ? index : start, index); }
    @Override public CharSequence getAccessibilityClassName() { return EditText.class.getName(); }
}
