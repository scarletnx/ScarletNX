// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.method;

/** How a text view's cursor and links react to keys and touch. */
public interface MovementMethod {
    default boolean canSelectArbitrarily() { return false; }
}
