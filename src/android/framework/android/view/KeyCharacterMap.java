// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** The characters of keys, for a US keyboard. */
public class KeyCharacterMap {
    public static final int VIRTUAL_KEYBOARD = -1, FULL = 4, ALPHA = 3, COMBINING_ACCENT = 0x80000000,
        COMBINING_ACCENT_MASK = 0x7fffffff;

    public static KeyCharacterMap load(int deviceId) { return new KeyCharacterMap(); }
    public static boolean deviceHasKey(int keyCode) { return true; }
    public int getKeyboardType() { return FULL; }
    public int get(int keyCode, int metaState) { return KeyEvent.charOf(keyCode, metaState); }
    public char getDisplayLabel(int keyCode) { return (char) Character.toUpperCase(KeyEvent.charOf(keyCode, 0)); }
    public char getNumber(int keyCode) { int c = KeyEvent.charOf(keyCode, 0); return c >= '0' && c <= '9' ? (char) c : 0; }
    public static int getDeadChar(int accent, int c) { return 0; }
    public KeyEvent[] getEvents(char[] chars) { return null; }
    public boolean isPrintingKey(int keyCode) { return KeyEvent.charOf(keyCode, 0) != 0; }

    @Deprecated public boolean getKeyData(int keyCode, KeyData results) {
        if (results.meta.length < KeyData.META_LENGTH) throw new IndexOutOfBoundsException("results.meta.length must be >= " + KeyData.META_LENGTH);
        char label = getDisplayLabel(keyCode);
        if (label == 0) return false;
        results.displayLabel = label;
        results.number = getNumber(keyCode);
        results.meta[0] = (char) get(keyCode, 0);
        results.meta[1] = (char) get(keyCode, KeyEvent.META_SHIFT_ON);
        results.meta[2] = (char) get(keyCode, KeyEvent.META_ALT_ON);
        results.meta[3] = (char) get(keyCode, KeyEvent.META_ALT_ON | KeyEvent.META_SHIFT_ON);
        return true;
    }

    @Deprecated
    public static class KeyData {
        public static final int META_LENGTH = 4;
        public char displayLabel, number;
        public char[] meta = new char[META_LENGTH];

        public KeyData() {}
    }

    public static class FallbackAction {
        public int keyCode, metaState;
    }
}
