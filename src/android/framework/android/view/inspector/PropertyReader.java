// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inspector;

/** Reads the values of the inspectable properties of an object, by their IDs. */
public interface PropertyReader {
    void readBoolean(int id, boolean value);
    void readByte(int id, byte value);
    void readChar(int id, char value);
    void readDouble(int id, double value);
    void readFloat(int id, float value);
    void readInt(int id, int value);
    void readLong(int id, long value);
    void readShort(int id, short value);
    void readObject(int id, Object value);
    void readColor(int id, int value);
    void readColor(int id, long value);
    void readColor(int id, android.graphics.Color value);
    void readGravity(int id, int value);
    void readIntEnum(int id, int value);
    void readIntFlag(int id, int value);
    void readResourceId(int id, int value);

    class PropertyTypeMismatchException extends RuntimeException {
        public PropertyTypeMismatchException(int id, String expectedPropertyType, String actualPropertyType, String propertyName) {
            super("Property " + (propertyName != null ? propertyName : "#" + id) + " is " + actualPropertyType + ", not " + expectedPropertyType);
        }
        public PropertyTypeMismatchException(int id, String expectedPropertyType, String actualPropertyType) {
            this(id, expectedPropertyType, actualPropertyType, null);
        }
    }
}
