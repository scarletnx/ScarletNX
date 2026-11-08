// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** A span that does not follow its text when the text is copied. */
public interface NoCopySpan {
    class Concrete implements NoCopySpan {}
}
