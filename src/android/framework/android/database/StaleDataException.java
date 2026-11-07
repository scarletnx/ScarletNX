// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** Thrown when a cursor's data is no longer there, as after its window closed. */
public class StaleDataException extends RuntimeException {
    public StaleDataException() {}
    public StaleDataException(String description) { super(description); }
}
