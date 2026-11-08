// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.method;

/** Movement that follows taps on links. */
public class LinkMovementMethod implements MovementMethod {
    private static final LinkMovementMethod INSTANCE = new LinkMovementMethod();

    public static MovementMethod getInstance() { return INSTANCE; }
}
