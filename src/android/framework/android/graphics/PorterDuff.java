// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** Porter-Duff compositing modes. */
public class PorterDuff {
    public enum Mode {
        CLEAR(0), SRC(1), DST(2), SRC_OVER(3), DST_OVER(4), SRC_IN(5), DST_IN(6), SRC_OUT(7), DST_OUT(8),
        SRC_ATOP(9), DST_ATOP(10), XOR(11), DARKEN(16), LIGHTEN(17), MULTIPLY(13), SCREEN(14), ADD(12), OVERLAY(15);

        public final int nativeInt;

        Mode(int nativeInt) { this.nativeInt = nativeInt; }
    }

    public static int modeToInt(Mode mode) { return mode.nativeInt; }
    public static Mode intToMode(int val) {
        for (Mode m : Mode.values()) if (m.nativeInt == val) return m;
        return Mode.CLEAR;
    }
}
