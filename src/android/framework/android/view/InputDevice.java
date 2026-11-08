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
    /** a gamepad's player number: none of these is one */
    public int getControllerNumber() { return 0; }

    /** An axis of a device's motions: its range in a source */
    public static final class MotionRange {
        private final int axis, source;
        private final float min, max, flat, fuzz, resolution;
        MotionRange(int axis, int source, float min, float max, float flat, float fuzz, float resolution) {
            this.axis = axis; this.source = source; this.min = min; this.max = max; this.flat = flat; this.fuzz = fuzz; this.resolution = resolution;
        }
        public int getAxis() { return axis; }
        public int getSource() { return source; }
        public boolean isFromSource(int source) { return (this.source & source) == source; }
        public float getMin() { return min; }
        public float getMax() { return max; }
        public float getRange() { return max - min; }
        public float getFlat() { return flat; }
        public float getFuzz() { return fuzz; }
        public float getResolution() { return resolution; }
    }
    /** the touchscreen's axes: x and y over the display, pressure and size from 0 to 1 */
    public java.util.List<MotionRange> getMotionRanges() {
        java.util.ArrayList<MotionRange> ranges = new java.util.ArrayList<>();
        if (id != TOUCH.id) return ranges;
        android.util.DisplayMetrics m = snx.android.Device.metrics();
        ranges.add(new MotionRange(MotionEvent.AXIS_X, SOURCE_TOUCHSCREEN, 0, m.widthPixels - 1, 0, 0, 0));
        ranges.add(new MotionRange(MotionEvent.AXIS_Y, SOURCE_TOUCHSCREEN, 0, m.heightPixels - 1, 0, 0, 0));
        ranges.add(new MotionRange(MotionEvent.AXIS_PRESSURE, SOURCE_TOUCHSCREEN, 0, 1, 0, 0, 0));
        ranges.add(new MotionRange(MotionEvent.AXIS_SIZE, SOURCE_TOUCHSCREEN, 0, 1, 0, 0, 0));
        return ranges;
    }
    public MotionRange getMotionRange(int axis) {
        for (MotionRange r : getMotionRanges()) if (r.getAxis() == axis) return r;
        return null;
    }
    public MotionRange getMotionRange(int axis, int source) {
        for (MotionRange r : getMotionRanges()) if (r.getAxis() == axis && r.isFromSource(source)) return r;
        return null;
    }
    /** the device's vibrators: none */
    private static final android.os.Vibrator NO_VIBRATOR = new android.os.Vibrator() {};
    public android.os.Vibrator getVibrator() { return NO_VIBRATOR; }
    public android.os.VibratorManager getVibratorManager() { return android.os.VibratorManager.none(NO_VIBRATOR); }
}
