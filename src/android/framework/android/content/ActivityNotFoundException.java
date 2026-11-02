// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

/** No activity handles an intent. */
public class ActivityNotFoundException extends RuntimeException {
    public ActivityNotFoundException() {}
    public ActivityNotFoundException(String name) { super(name); }
}
