// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

public interface HeaderIterator extends java.util.Iterator<Object> {
    boolean hasNext();
    Header nextHeader();
}
