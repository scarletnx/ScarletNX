// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** An input device: the window's mouse and keyboard, as a touchscreen and a keyboard. */
public final class InputDevice {
    public static final int SOURCE_CLASS_MASK = 0xff, SOURCE_CLASS_NONE = 0, SOURCE_CLASS_BUTTON = 1, SOURCE_CLASS_POINTER = 2,
        SOURCE_CLASS_TRACKBALL = 4, SOURCE_CLASS_POSITION = 8, SOURCE_CLASS_JOYSTICK = 16;
    public static final int SOURCE_UNKNOWN = 0, SOURCE_KEYBOARD = 0x101, SOURCE_DPAD = 0x201, SOURCE_GAMEPAD = 0x401,
        SOURCE_TOUCHSCREEN = 0x1002, SOURCE_MOUSE = 0x2002, SOURCE_STYLUS = 0x4002, SOURCE_TRACKBALL = 0x10004,
        SOURCE_MOUSE_RELATIVE = 0x20004, SOURCE_TOUCHPAD = 0x100008, SOURCE_JOYSTICK = 0x1000010,
        SOURCE_ROTARY_ENCODER = 0x400000, SOURCE_ANY = 0xffffff00;
    public static final int KEYBOARD_TYPE_NONE = 0, KEYBOARD_TYPE_NON_ALPHABETIC = 1, KEYBOARD_TYPE_ALPHABETIC = 2;
    private static final InputDevice TOUCH = new InputDevice(1, "touchscreen", SOURCE_TOUCHSCREEN | SOURCE_MOUSE);
    private static final InputDevice KEYBOARD = new InputDevice(-1, "keyboard", SOURCE_KEYBOARD);
    private final int id, sources;
    private final String name;

    private InputDevice(int id, String name, int sources) { this.id = id; this.name = name; this.sources = sources; }
    public static InputDevice getDevice(int id) { return id == TOUCH.id ? TOUCH : id == KEYBOARD.id ? KEYBOARD : null; }
    public static int[] getDeviceIds() { return new int[] {KEYBOARD.id, TOUCH.id}; }
    public int getId() { return id; }
    public String getName() { return name; }
    public int getSources() { return sources; }
    public boolean supportsSource(int source) { return (sources & source) == source; }
    public int getKeyboardType() { return id == KEYBOARD.id ? KEYBOARD_TYPE_ALPHABETIC : KEYBOARD_TYPE_NONE; }
    public KeyCharacterMap getKeyCharacterMap() { return KeyCharacterMap.load(id); }
    public boolean isVirtual() { return false; }
    public boolean isExternal() { return false; }
    public boolean isEnabled() { return true; }
    public int getVendorId() { return 0; }
    public int getProductId() { return 0; }
    public String getDescriptor() { return name; }
    public boolean[] hasKeys(int... keys) { return new boolean[keys.length]; }
}
