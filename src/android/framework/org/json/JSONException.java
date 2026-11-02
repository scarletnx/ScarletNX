// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.json;

/** A JSON text that can't be read, or a value of the wrong type. */
public class JSONException extends Exception {
    public JSONException(String message) { super(message); }
    public JSONException(String message, Throwable cause) { super(message, cause); }
    public JSONException(Throwable cause) { super(cause); }
}
