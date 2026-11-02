// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** A cursor read outside its rows. */
public class CursorIndexOutOfBoundsException extends IndexOutOfBoundsException {
    public CursorIndexOutOfBoundsException(int index, int size) { super("Index " + index + " requested, with a size of " + size); }
    public CursorIndexOutOfBoundsException(String desc) { super(desc); }
}
