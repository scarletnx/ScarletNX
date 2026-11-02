// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A key going down or up. */
public class KeyEvent extends InputEvent {
    public static final int KEYCODE_UNKNOWN = 0, KEYCODE_SOFT_LEFT = 1, KEYCODE_SOFT_RIGHT = 2, KEYCODE_HOME = 3, KEYCODE_BACK = 4,
        KEYCODE_CALL = 5, KEYCODE_ENDCALL = 6, KEYCODE_0 = 7, KEYCODE_1 = 8, KEYCODE_2 = 9, KEYCODE_3 = 10, KEYCODE_4 = 11,
        KEYCODE_5 = 12, KEYCODE_6 = 13, KEYCODE_7 = 14, KEYCODE_8 = 15, KEYCODE_9 = 16, KEYCODE_STAR = 17, KEYCODE_POUND = 18,
        KEYCODE_DPAD_UP = 19, KEYCODE_DPAD_DOWN = 20, KEYCODE_DPAD_LEFT = 21, KEYCODE_DPAD_RIGHT = 22, KEYCODE_DPAD_CENTER = 23,
        KEYCODE_VOLUME_UP = 24, KEYCODE_VOLUME_DOWN = 25, KEYCODE_POWER = 26, KEYCODE_CAMERA = 27, KEYCODE_CLEAR = 28,
        KEYCODE_A = 29, KEYCODE_B = 30, KEYCODE_C = 31, KEYCODE_D = 32, KEYCODE_E = 33, KEYCODE_F = 34, KEYCODE_G = 35,
        KEYCODE_H = 36, KEYCODE_I = 37, KEYCODE_J = 38, KEYCODE_K = 39, KEYCODE_L = 40, KEYCODE_M = 41, KEYCODE_N = 42,
        KEYCODE_O = 43, KEYCODE_P = 44, KEYCODE_Q = 45, KEYCODE_R = 46, KEYCODE_S = 47, KEYCODE_T = 48, KEYCODE_U = 49,
        KEYCODE_V = 50, KEYCODE_W = 51, KEYCODE_X = 52, KEYCODE_Y = 53, KEYCODE_Z = 54, KEYCODE_COMMA = 55, KEYCODE_PERIOD = 56,
        KEYCODE_ALT_LEFT = 57, KEYCODE_ALT_RIGHT = 58, KEYCODE_SHIFT_LEFT = 59, KEYCODE_SHIFT_RIGHT = 60, KEYCODE_TAB = 61,
        KEYCODE_SPACE = 62, KEYCODE_SYM = 63, KEYCODE_EXPLORER = 64, KEYCODE_ENVELOPE = 65, KEYCODE_ENTER = 66, KEYCODE_DEL = 67,
        KEYCODE_GRAVE = 68, KEYCODE_MINUS = 69, KEYCODE_EQUALS = 70, KEYCODE_LEFT_BRACKET = 71, KEYCODE_RIGHT_BRACKET = 72,
        KEYCODE_BACKSLASH = 73, KEYCODE_SEMICOLON = 74, KEYCODE_APOSTROPHE = 75, KEYCODE_SLASH = 76, KEYCODE_AT = 77,
        KEYCODE_NUM = 78, KEYCODE_HEADSETHOOK = 79, KEYCODE_FOCUS = 80, KEYCODE_PLUS = 81, KEYCODE_MENU = 82,
        KEYCODE_NOTIFICATION = 83, KEYCODE_SEARCH = 84, KEYCODE_PAGE_UP = 92, KEYCODE_PAGE_DOWN = 93, KEYCODE_ESCAPE = 111,
        KEYCODE_FORWARD_DEL = 112, KEYCODE_CTRL_LEFT = 113, KEYCODE_CTRL_RIGHT = 114, KEYCODE_CAPS_LOCK = 115,
        KEYCODE_META_LEFT = 117, KEYCODE_META_RIGHT = 118, KEYCODE_MOVE_HOME = 122, KEYCODE_MOVE_END = 123,
        KEYCODE_INSERT = 124, KEYCODE_F1 = 131, KEYCODE_NUMPAD_0 = 144, KEYCODE_NUMPAD_9 = 153, KEYCODE_NUMPAD_DIVIDE = 154,
        KEYCODE_NUMPAD_MULTIPLY = 155, KEYCODE_NUMPAD_SUBTRACT = 156, KEYCODE_NUMPAD_ADD = 157, KEYCODE_NUMPAD_DOT = 158,
        KEYCODE_NUMPAD_ENTER = 160, KEYCODE_NUMPAD_EQUALS = 161, KEYCODE_COPY = 278, KEYCODE_PASTE = 279, KEYCODE_CUT = 277;
    public static final int ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MULTIPLE = 2;
    public static final int META_SHIFT_ON = 0x1, META_ALT_ON = 0x2, META_SYM_ON = 0x4, META_FUNCTION_ON = 0x8,
        META_ALT_LEFT_ON = 0x10, META_ALT_RIGHT_ON = 0x20, META_SHIFT_LEFT_ON = 0x40, META_SHIFT_RIGHT_ON = 0x80,
        META_CTRL_ON = 0x1000, META_CTRL_LEFT_ON = 0x2000, META_CTRL_RIGHT_ON = 0x4000, META_META_ON = 0x10000,
        META_META_LEFT_ON = 0x20000, META_META_RIGHT_ON = 0x40000, META_CAPS_LOCK_ON = 0x100000,
        META_NUM_LOCK_ON = 0x200000, META_SCROLL_LOCK_ON = 0x400000, META_SHIFT_MASK = 0xc1, META_ALT_MASK = 0x32,
        META_CTRL_MASK = 0x7000, META_META_MASK = 0x70000;
    public static final int FLAG_WOKE_HERE = 0x1, FLAG_SOFT_KEYBOARD = 0x2, FLAG_KEEP_TOUCH_MODE = 0x4,
        FLAG_FROM_SYSTEM = 0x8, FLAG_EDITOR_ACTION = 0x10, FLAG_CANCELED = 0x20, FLAG_VIRTUAL_HARD_KEY = 0x40,
        FLAG_LONG_PRESS = 0x80, FLAG_CANCELED_LONG_PRESS = 0x100, FLAG_TRACKING = 0x200, FLAG_FALLBACK = 0x400;
    public static final int MAX_KEYCODE = 84;

    private int deviceId = -1, source = InputDevice.SOURCE_KEYBOARD, action, keyCode, repeatCount, metaState, scanCode, flags;
    private long downTime, eventTime;
    private String characters;

    public interface Callback {
        boolean onKeyDown(int keyCode, KeyEvent event);
        boolean onKeyLongPress(int keyCode, KeyEvent event);
        boolean onKeyUp(int keyCode, KeyEvent event);
        boolean onKeyMultiple(int keyCode, int count, KeyEvent event);
    }

    public static class DispatcherState {
        private int downKeyCode;
        private Object downTarget;

        public void reset() { downKeyCode = 0; downTarget = null; }
        public void reset(Object target) { if (downTarget == target) reset(); }
        public void startTracking(KeyEvent event, Object target) { downKeyCode = event.getKeyCode(); downTarget = target; }
        public boolean isTracking(KeyEvent event) { return downKeyCode == event.getKeyCode(); }
        public void performedLongPress(KeyEvent event) {}
        public void handleUpEvent(KeyEvent event) { if (downKeyCode == event.getKeyCode()) event.flags |= FLAG_TRACKING; reset(); }
    }

    public KeyEvent(int action, int code) { this(0, 0, action, code, 0); }
    public KeyEvent(long downTime, long eventTime, int action, int code, int repeat) { this(downTime, eventTime, action, code, repeat, 0); }
    public KeyEvent(long downTime, long eventTime, int action, int code, int repeat, int metaState) {
        this(downTime, eventTime, action, code, repeat, metaState, -1, 0, 0, InputDevice.SOURCE_KEYBOARD);
    }
    public KeyEvent(long downTime, long eventTime, int action, int code, int repeat, int metaState, int deviceId, int scancode) {
        this(downTime, eventTime, action, code, repeat, metaState, deviceId, scancode, 0, InputDevice.SOURCE_KEYBOARD);
    }
    public KeyEvent(long downTime, long eventTime, int action, int code, int repeat, int metaState, int deviceId, int scancode, int flags) {
        this(downTime, eventTime, action, code, repeat, metaState, deviceId, scancode, flags, InputDevice.SOURCE_KEYBOARD);
    }
    public KeyEvent(long downTime, long eventTime, int action, int code, int repeat, int metaState, int deviceId, int scancode, int flags, int source) {
        this.downTime = downTime; this.eventTime = eventTime; this.action = action; keyCode = code; repeatCount = repeat;
        this.metaState = metaState; this.deviceId = deviceId; scanCode = scancode; this.flags = flags; this.source = source;
    }
    public KeyEvent(long time, String characters, int deviceId, int flags) {
        this(time, time, ACTION_MULTIPLE, KEYCODE_UNKNOWN, 0, 0, deviceId, 0, flags);
        this.characters = characters;
    }
    public KeyEvent(KeyEvent o) {
        this(o.downTime, o.eventTime, o.action, o.keyCode, o.repeatCount, o.metaState, o.deviceId, o.scanCode, o.flags, o.source);
        characters = o.characters;
    }
    public KeyEvent(KeyEvent origEvent, long eventTime, int newRepeat) { this(origEvent); this.eventTime = eventTime; repeatCount = newRepeat; }
    public static KeyEvent changeAction(KeyEvent event, int action) { KeyEvent k = new KeyEvent(event); k.action = action; return k; }
    public static KeyEvent changeFlags(KeyEvent event, int flags) { KeyEvent k = new KeyEvent(event); k.flags = flags; return k; }
    public static KeyEvent changeTimeRepeat(KeyEvent event, long eventTime, int newRepeat) { return new KeyEvent(event, eventTime, newRepeat); }

    @Override public int getDeviceId() { return deviceId; }
    @Override public int getSource() { return source; }
    @Override public void setSource(int source) { this.source = source; }
    @Override public long getEventTime() { return eventTime; }
    @Override public long getEventTimeNanos() { return eventTime * 1000000L; }
    public final long getDownTime() { return downTime; }
    public final int getAction() { return action; }
    public final int getKeyCode() { return keyCode; }
    public final int getRepeatCount() { return repeatCount; }
    public final int getMetaState() { return metaState; }
    public final int getModifiers() { return metaState & (META_SHIFT_MASK | META_ALT_MASK | META_CTRL_MASK | META_META_MASK | META_SYM_ON | META_FUNCTION_ON); }
    public final int getScanCode() { return scanCode; }
    public final int getFlags() { return flags; }
    public final String getCharacters() { return characters; }
    public final boolean isCanceled() { return (flags & FLAG_CANCELED) != 0; }
    public final boolean isLongPress() { return (flags & FLAG_LONG_PRESS) != 0; }
    public final boolean isTracking() { return (flags & FLAG_TRACKING) != 0; }
    public final void startTracking() { flags |= FLAG_TRACKING; }
    public final boolean isSystem() { return keyCode == KEYCODE_BACK || keyCode == KEYCODE_HOME || keyCode == KEYCODE_MENU; }
    public final boolean isShiftPressed() { return (metaState & META_SHIFT_ON) != 0; }
    public final boolean isAltPressed() { return (metaState & META_ALT_ON) != 0; }
    public final boolean isCtrlPressed() { return (metaState & META_CTRL_ON) != 0; }
    public final boolean isMetaPressed() { return (metaState & META_META_ON) != 0; }
    public final boolean isFunctionPressed() { return (metaState & META_FUNCTION_ON) != 0; }
    public final boolean isSymPressed() { return (metaState & META_SYM_ON) != 0; }
    public final boolean isCapsLockOn() { return (metaState & META_CAPS_LOCK_ON) != 0; }
    public final boolean isNumLockOn() { return (metaState & META_NUM_LOCK_ON) != 0; }
    public final boolean hasNoModifiers() { return getModifiers() == 0; }
    public final boolean hasModifiers(int modifiers) { return getModifiers() == modifiers; }
    public static boolean metaStateHasNoModifiers(int metaState) { return (metaState & (META_SHIFT_MASK | META_ALT_MASK | META_CTRL_MASK | META_META_MASK)) == 0; }
    public static boolean metaStateHasModifiers(int metaState, int modifiers) { return (metaState & modifiers) == modifiers; }
    public static int normalizeMetaState(int metaState) { return metaState; }
    public static boolean isModifierKey(int keyCode) {
        return keyCode == KEYCODE_SHIFT_LEFT || keyCode == KEYCODE_SHIFT_RIGHT || keyCode == KEYCODE_ALT_LEFT || keyCode == KEYCODE_ALT_RIGHT
            || keyCode == KEYCODE_CTRL_LEFT || keyCode == KEYCODE_CTRL_RIGHT || keyCode == KEYCODE_META_LEFT || keyCode == KEYCODE_META_RIGHT
            || keyCode == KEYCODE_SYM || keyCode == KEYCODE_NUM;
    }
    public static boolean isGamepadButton(int keyCode) { return keyCode >= 96 && keyCode <= 110; }
    public static boolean isMediaSessionKey(int keyCode) { return false; }
    public static int getMaxKeyCode() { return 316; }
    public static int getDeadChar(int accent, int c) { return 0; }
    public static String keyCodeToString(int keyCode) { return "KEYCODE_" + keyCode; }
    public final KeyCharacterMap getKeyCharacterMap() { return KeyCharacterMap.load(deviceId); }
    public char getDisplayLabel() { return (char) Character.toUpperCase(charOf(keyCode, 0)); }
    public int getUnicodeChar() { return getUnicodeChar(metaState); }
    public int getUnicodeChar(int metaState) { return charOf(keyCode, metaState); }
    public char getNumber() { int c = charOf(keyCode, 0); return c >= '0' && c <= '9' ? (char) c : 0; }
    public boolean isPrintingKey() { return charOf(keyCode, 0) != 0; }
    public char getMatch(char[] chars) { return getMatch(chars, metaState); }
    public char getMatch(char[] chars, int metaState) {
        int c = charOf(keyCode, metaState);
        for (char x : chars) if (x == c) return x;
        return 0;
    }

    /** A US keyboard's character of a key, with shift. */
    static int charOf(int keyCode, int meta) {
        boolean shift = (meta & META_SHIFT_ON) != 0 ^ (meta & META_CAPS_LOCK_ON) != 0 && keyCode >= KEYCODE_A && keyCode <= KEYCODE_Z
            || (meta & META_SHIFT_ON) != 0 && !(keyCode >= KEYCODE_A && keyCode <= KEYCODE_Z);
        if (keyCode >= KEYCODE_A && keyCode <= KEYCODE_Z) return (shift ? 'A' : 'a') + keyCode - KEYCODE_A;
        if (keyCode >= KEYCODE_0 && keyCode <= KEYCODE_9) return shift ? ")!@#$%^&*(".charAt(keyCode - KEYCODE_0) : '0' + keyCode - KEYCODE_0;
        if (keyCode >= KEYCODE_NUMPAD_0 && keyCode <= KEYCODE_NUMPAD_9) return '0' + keyCode - KEYCODE_NUMPAD_0;
        switch (keyCode) {
            case KEYCODE_SPACE: return ' ';
            case KEYCODE_COMMA: return shift ? '<' : ',';
            case KEYCODE_PERIOD: return shift ? '>' : '.';
            case KEYCODE_MINUS: return shift ? '_' : '-';
            case KEYCODE_EQUALS: return shift ? '+' : '=';
            case KEYCODE_LEFT_BRACKET: return shift ? '{' : '[';
            case KEYCODE_RIGHT_BRACKET: return shift ? '}' : ']';
            case KEYCODE_BACKSLASH: return shift ? '|' : '\\';
            case KEYCODE_SEMICOLON: return shift ? ':' : ';';
            case KEYCODE_APOSTROPHE: return shift ? '"' : '\'';
            case KEYCODE_SLASH: return shift ? '?' : '/';
            case KEYCODE_GRAVE: return shift ? '~' : '`';
            case KEYCODE_TAB: return '\t';
            case KEYCODE_ENTER: case KEYCODE_NUMPAD_ENTER: return '\n';
            case KEYCODE_STAR: case KEYCODE_NUMPAD_MULTIPLY: return '*';
            case KEYCODE_POUND: return '#';
            case KEYCODE_PLUS: case KEYCODE_NUMPAD_ADD: return '+';
            case KEYCODE_NUMPAD_SUBTRACT: return '-';
            case KEYCODE_NUMPAD_DIVIDE: return '/';
            case KEYCODE_NUMPAD_DOT: return '.';
            case KEYCODE_NUMPAD_EQUALS: return '=';
            case KEYCODE_AT: return '@';
            default: return 0;
        }
    }

    /** The event to a receiver's key callbacks, as Android's KeyEvent.dispatch does. */
    public final boolean dispatch(Callback receiver, DispatcherState state, Object target) {
        switch (action) {
            case ACTION_DOWN: {
                flags &= ~FLAG_START_TRACKING;
                boolean res = receiver.onKeyDown(keyCode, this);
                if (state != null && res && repeatCount == 0 && (flags & FLAG_START_TRACKING) != 0) state.startTracking(this, target);
                return res;
            }
            case ACTION_UP:
                if (state != null) state.handleUpEvent(this);
                return receiver.onKeyUp(keyCode, this);
            case ACTION_MULTIPLE:
                return receiver.onKeyMultiple(keyCode, repeatCount, this);
            default:
                return false;
        }
    }
    @Deprecated public final boolean dispatch(Callback receiver) { return dispatch(receiver, null, null); }
    private static final int FLAG_START_TRACKING = 0x40000000;
    @Override public String toString() { return "KeyEvent { action=" + action + ", keyCode=" + keyCode + ", metaState=0x" + Integer.toHexString(metaState) + " }"; }
}
