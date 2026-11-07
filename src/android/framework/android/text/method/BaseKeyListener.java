// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.method;

import android.text.Editable;
import android.text.Selection;
import android.text.TextUtils;
import android.view.KeyEvent;
import android.view.View;

/** The keys that delete text, for the key listeners. */
public abstract class BaseKeyListener extends MetaKeyKeyListener implements KeyListener {
    public BaseKeyListener() {}

    public boolean backspace(View view, Editable content, int keyCode, KeyEvent event) { return delete(content, false); }
    public boolean forwardDelete(View view, Editable content, int keyCode, KeyEvent event) { return delete(content, true); }
    private static boolean delete(Editable content, boolean forward) {
        int a = Selection.getSelectionStart(content), b = Selection.getSelectionEnd(content);
        if (a < 0 || b < 0) return false;
        int start = Math.min(a, b), end = Math.max(a, b);
        if (start == end) {
            if (forward) end = TextUtils.getOffsetAfter(content, start);
            else start = TextUtils.getOffsetBefore(content, start);
            if (start == end) return false;
        }
        content.delete(start, end);
        return true;
    }
    @Override public boolean onKeyDown(View view, Editable content, int keyCode, KeyEvent event) {
        boolean handled;
        if (keyCode == KeyEvent.KEYCODE_DEL) handled = backspace(view, content, keyCode, event);
        else if (keyCode == KeyEvent.KEYCODE_FORWARD_DEL) handled = forwardDelete(view, content, keyCode, event);
        else return super.onKeyDown(view, content, keyCode, event);
        if (handled) adjustMetaAfterKeypress(content);
        return handled;
    }
    @Override public boolean onKeyOther(View view, Editable content, KeyEvent event) { return false; }
}
