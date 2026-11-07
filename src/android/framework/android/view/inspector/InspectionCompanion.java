// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inspector;

/** Maps and reads the properties of a class for the layout inspector. No inspector runs here. */
public interface InspectionCompanion<T> {
    void mapProperties(PropertyMapper propertyMapper);
    void readProperties(T inspectable, PropertyReader propertyReader);

    class UninitializedPropertyMapException extends RuntimeException {
        public UninitializedPropertyMapException() { super("Unable to read properties of an inspectable before mapping their IDs."); }
    }
}
