// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

/** A header of a message: its name and value, and the elements of its value. */
public interface Header {
    String getName();
    String getValue();
    HeaderElement[] getElements() throws ParseException;
}
