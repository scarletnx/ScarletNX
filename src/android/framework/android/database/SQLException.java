// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** An SQL error. */
public class SQLException extends RuntimeException {
    public SQLException() {}
    public SQLException(String error) { super(error); }
    public SQLException(String error, Throwable cause) { super(error, cause); }
}
