// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.method;

import android.text.Editable;
import android.view.KeyEvent;
import android.view.View;

/** Turns key events into edits of a text. */
public interface KeyListener {
    int getInputType();

    boolean onKeyDown(View view, Editable text, int keyCode, KeyEvent event);

    boolean onKeyUp(View view, Editable text, int keyCode, KeyEvent event);

    boolean onKeyOther(View view, Editable text, KeyEvent event);

    void clearMetaKeyState(View view, Editable content, int states);
}
