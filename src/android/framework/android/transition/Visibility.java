// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.transition;

/** A transition of views that appear or disappear; here they do at once. */
public abstract class Visibility extends Transition {
    public static final int MODE_IN = 0x1, MODE_OUT = 0x2;
    private int mode = MODE_IN | MODE_OUT;

    public Visibility() {}
    public Visibility(android.content.Context context, android.util.AttributeSet attrs) { super(context, attrs); }
    public void setMode(int mode) {
        if ((mode & ~(MODE_IN | MODE_OUT)) != 0) throw new IllegalArgumentException("Only MODE_IN and MODE_OUT flags are allowed");
        this.mode = mode;
    }
    public int getMode() { return mode; }
}
