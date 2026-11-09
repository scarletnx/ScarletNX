// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.transition;

/** Fades views in or out; here they show or hide at once. */
public class Fade extends Visibility {
    public static final int IN = Visibility.MODE_IN, OUT = Visibility.MODE_OUT;

    public Fade() {}
    public Fade(int fadingMode) { setMode(fadingMode); }
    public Fade(android.content.Context context, android.util.AttributeSet attrs) { super(context, attrs); }
}
