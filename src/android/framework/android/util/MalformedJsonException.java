// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** JSON text a JsonReader can't read */
public final class MalformedJsonException extends java.io.IOException {
    public MalformedJsonException(String message) { super(message); }
}
