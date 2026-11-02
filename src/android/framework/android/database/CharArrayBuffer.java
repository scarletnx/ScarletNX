// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** A buffer of chars a cursor copies a string into. */
public final class CharArrayBuffer {
    public char[] data;
    public int sizeCopied;
    public CharArrayBuffer(int size) { data = new char[size]; }
    public CharArrayBuffer(char[] buf) { data = buf; }
}
