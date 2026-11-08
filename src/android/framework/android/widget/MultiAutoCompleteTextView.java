// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.text.Editable;
import android.text.Selection;
import android.util.AttributeSet;

/** An auto-complete text view that completes each token of its text: a tokenizer finds the token at the cursor. */
public class MultiAutoCompleteTextView extends AutoCompleteTextView {
    private Tokenizer tokenizer;

    public interface Tokenizer {
        int findTokenStart(CharSequence text, int cursor);
        int findTokenEnd(CharSequence text, int cursor);
        CharSequence terminateToken(CharSequence text);
    }

    /** Tokens separated by commas and the spaces after them. */
    public static class CommaTokenizer implements Tokenizer {
        public CommaTokenizer() {}
        @Override public int findTokenStart(CharSequence text, int cursor) {
            int i = cursor;
            while (i > 0 && text.charAt(i - 1) != ',') --i;
            while (i < cursor && text.charAt(i) == ' ') ++i;
            return i;
        }
        @Override public int findTokenEnd(CharSequence text, int cursor) {
            for (int i = cursor; i < text.length(); ++i) if (text.charAt(i) == ',') return i;
            return text.length();
        }
        @Override public CharSequence terminateToken(CharSequence text) {
            int i = text.length();
            while (i > 0 && text.charAt(i - 1) == ' ') --i;
            if (i > 0 && text.charAt(i - 1) == ',') return text;
            return text + ", ";
        }
    }

    public MultiAutoCompleteTextView(Context context) { this(context, null); }
    public MultiAutoCompleteTextView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public MultiAutoCompleteTextView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public MultiAutoCompleteTextView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public void setTokenizer(Tokenizer t) { tokenizer = t; }
    private int cursor() { int end = getSelectionEnd(); return end < 0 ? getText().length() : end; }
    @Override public boolean enoughToFilter() {
        if (tokenizer == null) return super.enoughToFilter();
        Editable text = getText();
        int end = cursor();
        return end - tokenizer.findTokenStart(text, end) >= getThreshold();
    }
    @Override protected void performFiltering(CharSequence text, int keyCode) {
        if (tokenizer == null || !enoughToFilter()) { if (tokenizer == null) super.performFiltering(text, keyCode); else dismissDropDown(); return; }
        int end = cursor();
        performFiltering(text, tokenizer.findTokenStart(text, end), end, keyCode);
    }
    protected void performFiltering(CharSequence text, int start, int end, int keyCode) { getFilter().filter(text.subSequence(start, end), this); }
    @Override public void performValidation() {
        if (tokenizer == null) super.performValidation();
    }
    @Override protected void replaceText(CharSequence text) {
        if (tokenizer == null) { super.replaceText(text); return; }
        Editable e = getText();
        int end = cursor(), start = tokenizer.findTokenStart(e, end);
        e.replace(start, end, tokenizer.terminateToken(text));
        Selection.setSelection(e, Math.min(e.length(), start + tokenizer.terminateToken(text).length()));
    }
    @Override public CharSequence getAccessibilityClassName() { return MultiAutoCompleteTextView.class.getName(); }
}
