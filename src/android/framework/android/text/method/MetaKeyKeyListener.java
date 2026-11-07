// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.text.method;

import android.text.Editable;
import android.text.NoCopySpan;
import android.text.Spannable;
import android.text.Spanned;
import android.view.KeyEvent;
import android.view.View;

/**
 * The state of the shift, alt and sym keys of a text, kept as spans on it. A meta key that is pressed and released
 * alone stays on for the next key. Two such presses lock it, and a third unlocks it.
 */
public abstract class MetaKeyKeyListener {
    public static final int META_SHIFT_ON = KeyEvent.META_SHIFT_ON, META_ALT_ON = KeyEvent.META_ALT_ON, META_SYM_ON = KeyEvent.META_SYM_ON;
    public static final int META_CAP_LOCKED = 0x100, META_ALT_LOCKED = 0x200, META_SYM_LOCKED = 0x400;

    private static final Object CAP = new NoCopySpan.Concrete(), ALT = new NoCopySpan.Concrete(), SYM = new NoCopySpan.Concrete();
    private static final int PRESSED = Spanned.SPAN_MARK_MARK | (1 << Spanned.SPAN_USER_SHIFT);
    private static final int RELEASED = Spanned.SPAN_MARK_MARK | (2 << Spanned.SPAN_USER_SHIFT);
    private static final int USED = Spanned.SPAN_MARK_MARK | (3 << Spanned.SPAN_USER_SHIFT);
    private static final int LOCKED = Spanned.SPAN_MARK_MARK | (4 << Spanned.SPAN_USER_SHIFT);

    public MetaKeyKeyListener() {}

    private static int state(CharSequence text, Object what, int on, int locked) {
        if (!(text instanceof Spanned sp)) return 0;
        int flags = sp.getSpanFlags(what);
        return flags == LOCKED ? locked : flags != 0 ? on : 0;
    }
    public static final int getMetaState(CharSequence text) {
        return state(text, CAP, META_SHIFT_ON, META_CAP_LOCKED) | state(text, ALT, META_ALT_ON, META_ALT_LOCKED)
            | state(text, SYM, META_SYM_ON, META_SYM_LOCKED);
    }
    public static final int getMetaState(CharSequence text, KeyEvent event) {
        int meta = event.getMetaState();
        return text instanceof Spanned ? meta | getMetaState(text) : meta;
    }
    public static final int getMetaState(CharSequence text, int meta) {
        switch (meta) {
            case META_SHIFT_ON: return state(text, CAP, 1, 2);
            case META_ALT_ON: return state(text, ALT, 1, 2);
            case META_SYM_ON: return state(text, SYM, 1, 2);
            default: return 0;
        }
    }
    /** After a key that is not a meta key: a meta key that was on for one key goes off, and a held one counts as used. */
    public static void adjustMetaAfterKeypress(Spannable content) {
        adjust(content, CAP);
        adjust(content, ALT);
        adjust(content, SYM);
    }
    private static void adjust(Spannable content, Object what) {
        int flags = content.getSpanFlags(what);
        if (flags == PRESSED) content.setSpan(what, 0, 0, USED);
        else if (flags == RELEASED) content.removeSpan(what);
    }
    public static boolean isMetaTracker(CharSequence text, Object what) { return what == CAP || what == ALT || what == SYM; }
    public static void resetMetaState(Spannable text) {
        text.removeSpan(CAP);
        text.removeSpan(ALT);
        text.removeSpan(SYM);
    }
    protected static void resetLockedMeta(Spannable content) {
        for (Object what : new Object[] {CAP, ALT, SYM}) if (content.getSpanFlags(what) == LOCKED) content.removeSpan(what);
    }

    private static Object tracker(int keyCode) {
        if (keyCode == KeyEvent.KEYCODE_SHIFT_LEFT || keyCode == KeyEvent.KEYCODE_SHIFT_RIGHT) return CAP;
        if (keyCode == KeyEvent.KEYCODE_ALT_LEFT || keyCode == KeyEvent.KEYCODE_ALT_RIGHT) return ALT;
        return keyCode == KeyEvent.KEYCODE_SYM ? SYM : null;
    }
    public boolean onKeyDown(View view, Editable content, int keyCode, KeyEvent event) {
        Object what = tracker(keyCode);
        if (what == null) return false;
        int flags = content.getSpanFlags(what);
        if (flags == RELEASED) content.setSpan(what, 0, 0, LOCKED);
        else if (flags == LOCKED) content.removeSpan(what);
        else if (flags != PRESSED && flags != USED) content.setSpan(what, 0, 0, PRESSED);
        return true;
    }
    public boolean onKeyUp(View view, Editable content, int keyCode, KeyEvent event) {
        Object what = tracker(keyCode);
        if (what == null) return false;
        int flags = content.getSpanFlags(what);
        if (flags == USED) content.removeSpan(what);
        else if (flags == PRESSED) content.setSpan(what, 0, 0, RELEASED);
        return true;
    }
    public void clearMetaKeyState(View view, Editable content, int states) { clearMetaKeyState(content, states); }
    public static void clearMetaKeyState(Editable content, int states) {
        if ((states & META_SHIFT_ON) != 0) content.removeSpan(CAP);
        if ((states & META_ALT_ON) != 0) content.removeSpan(ALT);
        if ((states & META_SYM_ON) != 0) content.removeSpan(SYM);
    }
}
