// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Matrix;

/** A touch, mouse or stylus event: one or more pointers, without history. */
public final class MotionEvent extends InputEvent {
    public static final int ACTION_MASK = 0xff, ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3,
        ACTION_OUTSIDE = 4, ACTION_POINTER_DOWN = 5, ACTION_POINTER_UP = 6, ACTION_HOVER_MOVE = 7, ACTION_SCROLL = 8,
        ACTION_HOVER_ENTER = 9, ACTION_HOVER_EXIT = 10, ACTION_BUTTON_PRESS = 11, ACTION_BUTTON_RELEASE = 12;
    public static final int ACTION_POINTER_INDEX_MASK = 0xff00, ACTION_POINTER_INDEX_SHIFT = 8;
    public static final int AXIS_X = 0, AXIS_Y = 1, AXIS_PRESSURE = 2, AXIS_SIZE = 3, AXIS_TOUCH_MAJOR = 4,
        AXIS_TOUCH_MINOR = 5, AXIS_TOOL_MAJOR = 6, AXIS_TOOL_MINOR = 7, AXIS_ORIENTATION = 8, AXIS_VSCROLL = 9,
        AXIS_HSCROLL = 10, AXIS_Z = 11, AXIS_RX = 12, AXIS_RY = 13, AXIS_RZ = 14, AXIS_SCROLL = 26,
        AXIS_RELATIVE_X = 27, AXIS_RELATIVE_Y = 28, AXIS_GESTURE_X_OFFSET = 48, AXIS_GESTURE_Y_OFFSET = 49,
        AXIS_GESTURE_SCROLL_X_DISTANCE = 50, AXIS_GESTURE_SCROLL_Y_DISTANCE = 51, AXIS_GESTURE_PINCH_SCALE_FACTOR = 52;
    public static final int TOOL_TYPE_UNKNOWN = 0, TOOL_TYPE_FINGER = 1, TOOL_TYPE_STYLUS = 2, TOOL_TYPE_MOUSE = 3,
        TOOL_TYPE_ERASER = 4, TOOL_TYPE_PALM = 5;
    public static final int BUTTON_PRIMARY = 1, BUTTON_SECONDARY = 2, BUTTON_TERTIARY = 4, BUTTON_BACK = 8,
        BUTTON_FORWARD = 16, BUTTON_STYLUS_PRIMARY = 32, BUTTON_STYLUS_SECONDARY = 64;
    public static final int FLAG_WINDOW_IS_OBSCURED = 0x1, FLAG_WINDOW_IS_PARTIALLY_OBSCURED = 0x2,
        FLAG_CANCELED = 0x20;
    public static final int EDGE_TOP = 1, EDGE_BOTTOM = 2, EDGE_LEFT = 4, EDGE_RIGHT = 8;
    public static final int CLASSIFICATION_NONE = 0, CLASSIFICATION_AMBIGUOUS_GESTURE = 1, CLASSIFICATION_DEEP_PRESS = 2,
        CLASSIFICATION_TWO_FINGER_SWIPE = 3, CLASSIFICATION_MULTI_FINGER_SWIPE = 4, CLASSIFICATION_PINCH = 5;
    public static final int INVALID_POINTER_ID = -1;

    public static final class PointerCoords {
        public float x, y, pressure = 1, size, touchMajor, touchMinor, toolMajor, toolMinor, orientation;
        public boolean isResampled() { return false; }
        public PointerCoords() {}
        public PointerCoords(PointerCoords o) { copyFrom(o); }
        public void clear() { x = y = size = touchMajor = touchMinor = toolMajor = toolMinor = orientation = 0; pressure = 1; scroll = hscroll = 0; }
        public void copyFrom(PointerCoords o) {
            x = o.x; y = o.y; pressure = o.pressure; size = o.size; touchMajor = o.touchMajor; touchMinor = o.touchMinor;
            toolMajor = o.toolMajor; toolMinor = o.toolMinor; orientation = o.orientation; scroll = o.scroll; hscroll = o.hscroll;
        }
        float scroll, hscroll;
        public float getAxisValue(int axis) {
            switch (axis) {
                case AXIS_X: return x;
                case AXIS_Y: return y;
                case AXIS_PRESSURE: return pressure;
                case AXIS_SIZE: return size;
                case AXIS_TOUCH_MAJOR: return touchMajor;
                case AXIS_TOUCH_MINOR: return touchMinor;
                case AXIS_TOOL_MAJOR: return toolMajor;
                case AXIS_TOOL_MINOR: return toolMinor;
                case AXIS_ORIENTATION: return orientation;
                case AXIS_VSCROLL: return scroll;
                case AXIS_HSCROLL: return hscroll;
                default: return 0;
            }
        }
        public void setAxisValue(int axis, float value) {
            switch (axis) {
                case AXIS_X: x = value; break;
                case AXIS_Y: y = value; break;
                case AXIS_PRESSURE: pressure = value; break;
                case AXIS_SIZE: size = value; break;
                case AXIS_VSCROLL: scroll = value; break;
                case AXIS_HSCROLL: hscroll = value; break;
                default: break;
            }
        }
    }

    public static final class PointerProperties {
        public int id, toolType;
        public PointerProperties() {}
        public PointerProperties(PointerProperties o) { copyFrom(o); }
        public void clear() { id = INVALID_POINTER_ID; toolType = TOOL_TYPE_UNKNOWN; }
        public void copyFrom(PointerProperties o) { id = o.id; toolType = o.toolType; }
    }

    private long downTime, eventTime;
    private int action, metaState, buttonState, flags, edgeFlags, deviceId = 1, source = InputDevice.SOURCE_TOUCHSCREEN,
        actionButton, classification;
    private float xPrecision = 1, yPrecision = 1, rawOffsetX, rawOffsetY;
    private PointerProperties[] props;
    private PointerCoords[] coords;

    private MotionEvent() {}

    public static MotionEvent obtain(long downTime, long eventTime, int action, int pointerCount, PointerProperties[] pointerProperties,
                                     PointerCoords[] pointerCoords, int metaState, int buttonState, float xPrecision, float yPrecision,
                                     int deviceId, int edgeFlags, int source, int flags) {
        MotionEvent e = new MotionEvent();
        e.downTime = downTime; e.eventTime = eventTime; e.action = action; e.metaState = metaState; e.buttonState = buttonState;
        e.xPrecision = xPrecision; e.yPrecision = yPrecision; e.deviceId = deviceId; e.edgeFlags = edgeFlags; e.source = source; e.flags = flags;
        e.props = new PointerProperties[pointerCount];
        e.coords = new PointerCoords[pointerCount];
        for (int i = 0; i < pointerCount; ++i) {
            e.props[i] = new PointerProperties(pointerProperties[i]);
            e.coords[i] = new PointerCoords(pointerCoords[i]);
        }
        return e;
    }
    public static MotionEvent obtain(long downTime, long eventTime, int action, int pointerCount, PointerProperties[] pointerProperties,
                                     PointerCoords[] pointerCoords, int metaState, int buttonState, float xPrecision, float yPrecision,
                                     int deviceId, int edgeFlags, int source, int displayId, int flags, int classification) {
        MotionEvent e = obtain(downTime, eventTime, action, pointerCount, pointerProperties, pointerCoords, metaState, buttonState,
            xPrecision, yPrecision, deviceId, edgeFlags, source, flags);
        e.classification = classification;
        return e;
    }
    public static MotionEvent obtain(long downTime, long eventTime, int action, float x, float y, float pressure, float size,
                                     int metaState, float xPrecision, float yPrecision, int deviceId, int edgeFlags) {
        PointerProperties p = new PointerProperties();
        p.toolType = TOOL_TYPE_FINGER;
        PointerCoords c = new PointerCoords();
        c.x = x; c.y = y; c.pressure = pressure; c.size = size;
        return obtain(downTime, eventTime, action, 1, new PointerProperties[] {p}, new PointerCoords[] {c}, metaState, 0,
            xPrecision, yPrecision, deviceId, edgeFlags, InputDevice.SOURCE_TOUCHSCREEN, 0);
    }
    public static MotionEvent obtain(long downTime, long eventTime, int action, float x, float y, int metaState) {
        return obtain(downTime, eventTime, action, x, y, 1, 1, metaState, 1, 1, 1, 0);
    }
    public static MotionEvent obtain(MotionEvent o) {
        MotionEvent e = obtain(o.downTime, o.eventTime, o.action, o.props.length, o.props, o.coords, o.metaState, o.buttonState,
            o.xPrecision, o.yPrecision, o.deviceId, o.edgeFlags, o.source, o.flags);
        e.actionButton = o.actionButton;
        e.classification = o.classification;
        e.rawOffsetX = o.rawOffsetX;
        e.rawOffsetY = o.rawOffsetY;
        return e;
    }
    public static MotionEvent obtainNoHistory(MotionEvent o) { return obtain(o); }
    public MotionEvent copy() { return obtain(this); }
    @Override public void recycle() {}

    @Override public int getDeviceId() { return deviceId; }
    @Override public int getSource() { return source; }
    @Override public void setSource(int source) { this.source = source; }
    @Override public long getEventTime() { return eventTime; }
    @Override public long getEventTimeNanos() { return eventTime * 1000000L; }
    public long getDownTime() { return downTime; }
    public void setDownTime(long downTime) { this.downTime = downTime; }
    public int getAction() { return action; }
    public int getActionMasked() { return action & ACTION_MASK; }
    public int getActionIndex() { return (action & ACTION_POINTER_INDEX_MASK) >> ACTION_POINTER_INDEX_SHIFT; }
    public void setAction(int action) { this.action = action; }
    public int getActionButton() { return actionButton; }
    public void setActionButton(int button) { actionButton = button; }
    public int getButtonState() { return buttonState; }
    public void setButtonState(int buttonState) { this.buttonState = buttonState; }
    public boolean isButtonPressed(int button) { return button != 0 && (buttonState & button) == button; }
    public int getMetaState() { return metaState; }
    public int getFlags() { return flags; }
    public int getEdgeFlags() { return edgeFlags; }
    public void setEdgeFlags(int flags) { edgeFlags = flags; }
    public int getClassification() { return classification; }
    public boolean isTouchEvent() { int a = getActionMasked(); return a == ACTION_DOWN || a == ACTION_MOVE || a == ACTION_UP || a == ACTION_POINTER_DOWN || a == ACTION_POINTER_UP || a == ACTION_CANCEL || a == ACTION_OUTSIDE; }
    public int getPointerCount() { return props.length; }
    public int getPointerId(int pointerIndex) { return props[pointerIndex].id; }
    public int getToolType(int pointerIndex) { return props[pointerIndex].toolType; }
    public int findPointerIndex(int pointerId) { for (int i = 0; i < props.length; ++i) if (props[i].id == pointerId) return i; return -1; }
    public float getX() { return coords[0].x; }
    public float getY() { return coords[0].y; }
    public float getPressure() { return coords[0].pressure; }
    public float getSize() { return coords[0].size; }
    public float getTouchMajor() { return coords[0].touchMajor; }
    public float getTouchMinor() { return coords[0].touchMinor; }
    public float getToolMajor() { return coords[0].toolMajor; }
    public float getToolMinor() { return coords[0].toolMinor; }
    public float getOrientation() { return coords[0].orientation; }
    public float getAxisValue(int axis) { return coords[0].getAxisValue(axis); }
    public float getX(int i) { return coords[i].x; }
    public float getY(int i) { return coords[i].y; }
    public float getPressure(int i) { return coords[i].pressure; }
    public float getSize(int i) { return coords[i].size; }
    public float getTouchMajor(int i) { return coords[i].touchMajor; }
    public float getTouchMinor(int i) { return coords[i].touchMinor; }
    public float getToolMajor(int i) { return coords[i].toolMajor; }
    public float getToolMinor(int i) { return coords[i].toolMinor; }
    public float getOrientation(int i) { return coords[i].orientation; }
    public float getAxisValue(int axis, int i) { return coords[i].getAxisValue(axis); }
    public void getPointerCoords(int i, PointerCoords out) { out.copyFrom(coords[i]); }
    public void getPointerProperties(int i, PointerProperties out) { out.copyFrom(props[i]); }
    public float getRawX() { return coords[0].x + rawOffsetX; }
    public float getRawY() { return coords[0].y + rawOffsetY; }
    public float getRawX(int i) { return coords[i].x + rawOffsetX; }
    public float getRawY(int i) { return coords[i].y + rawOffsetY; }
    public float getXPrecision() { return xPrecision; }
    public float getYPrecision() { return yPrecision; }
    public int getHistorySize() { return 0; }
    public long getHistoricalEventTime(int pos) { return eventTime; }
    public long getHistoricalEventTimeNanos(int pos) { return getEventTimeNanos(); }
    public float getHistoricalX(int pos) { return getX(); }
    public float getHistoricalY(int pos) { return getY(); }
    public float getHistoricalX(int pointerIndex, int pos) { return getX(pointerIndex); }
    public float getHistoricalY(int pointerIndex, int pos) { return getY(pointerIndex); }
    public float getHistoricalAxisValue(int axis, int pointerIndex, int pos) { return getAxisValue(axis, pointerIndex); }
    public float getHistoricalAxisValue(int axis, int pos) { return getAxisValue(axis); }
    public void getHistoricalPointerCoords(int pointerIndex, int pos, PointerCoords out) { getPointerCoords(pointerIndex, out); }
    public void setLocation(float x, float y) { offsetLocation(x - getX(), y - getY()); }
    public void offsetLocation(float deltaX, float deltaY) {
        for (PointerCoords c : coords) { c.x += deltaX; c.y += deltaY; }
        rawOffsetX -= deltaX;
        rawOffsetY -= deltaY;
    }
    public void transform(Matrix matrix) {
        float[] p = new float[2];
        for (PointerCoords c : coords) {
            p[0] = c.x; p[1] = c.y;
            matrix.mapPoints(p);
            c.x = p[0]; c.y = p[1];
        }
    }
    public void applyTransform(Matrix matrix) { transform(matrix); }
    public void addBatch(long eventTime, float x, float y, float pressure, float size, int metaState) {
        this.eventTime = eventTime;
        coords[0].x = x; coords[0].y = y; coords[0].pressure = pressure; coords[0].size = size;
    }
    public void addBatch(long eventTime, PointerCoords[] pointerCoords, int metaState) {
        this.eventTime = eventTime;
        for (int i = 0; i < coords.length && i < pointerCoords.length; ++i) coords[i].copyFrom(pointerCoords[i]);
    }
    public static String actionToString(int action) { return "ACTION_" + action; }
    public static String axisToString(int axis) { return "AXIS_" + axis; }
    @Override public String toString() {
        return "MotionEvent { action=" + action + ", x=" + getX() + ", y=" + getY() + ", pointers=" + props.length + ", time=" + eventTime + " }";
    }
}
