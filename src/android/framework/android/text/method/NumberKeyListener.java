// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.method;

import android.text.Editable;
import android.text.InputFilter;
import android.text.Selection;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.view.KeyEvent;
import android.view.View;

/** A key listener and an input filter that accept only the chars of getAcceptedChars. */
public abstract class NumberKeyListener extends BaseKeyListener implements InputFilter {
    public NumberKeyListener() {}

    protected abstract char[] getAcceptedChars();

    protected int lookup(KeyEvent event, android.text.Spannable content) { return event.getMatch(getAcceptedChars(), getMetaState(content, event)); }
    protected static boolean ok(char[] accept, char c) {
        for (char a : accept) if (a == c) return true;
        return false;
    }
    @Override public CharSequence filter(CharSequence source, int start, int end, Spanned dest, int dstart, int dend) {
        char[] accept = getAcceptedChars();
        int i = start;
        while (i < end && ok(accept, source.charAt(i))) ++i;
        if (i == end) return null;
        if (end - start == 1) return "";
        SpannableStringBuilder filtered = new SpannableStringBuilder(source, start, end);
        for (int k = end - start - 1; k >= i - start; --k) if (!ok(accept, source.charAt(start + k))) filtered.delete(k, k + 1);
        return filtered;
    }
    @Override public boolean onKeyDown(View view, Editable content, int keyCode, KeyEvent event) {
        int a = Selection.getSelectionStart(content), b = Selection.getSelectionEnd(content);
        int start = Math.max(0, Math.min(a, b)), end = Math.max(0, Math.max(a, b));
        int c = event != null ? lookup(event, content) : 0;
        if (c != 0 && ok(getAcceptedChars(), (char) c)) {
            content.replace(start, end, String.valueOf((char) c));
            adjustMetaAfterKeypress(content);
            return true;
        }
        return super.onKeyDown(view, content, keyCode, event);
    }
}
