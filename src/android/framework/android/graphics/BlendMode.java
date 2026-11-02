// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** Blend modes; their numbers are Skia's SkBlendMode, which the display list carries. */
public enum BlendMode {
    CLEAR(0), SRC(1), DST(2), SRC_OVER(3), DST_OVER(4), SRC_IN(5), DST_IN(6), SRC_OUT(7), DST_OUT(8), SRC_ATOP(9),
    DST_ATOP(10), XOR(11), PLUS(12), MODULATE(13), SCREEN(14), OVERLAY(15), DARKEN(16), LIGHTEN(17), COLOR_DODGE(18),
    COLOR_BURN(19), HARD_LIGHT(20), SOFT_LIGHT(21), DIFFERENCE(22), EXCLUSION(23), MULTIPLY(24), HUE(25),
    SATURATION(26), COLOR(27), LUMINOSITY(28);

    final int skia;

    BlendMode(int skia) { this.skia = skia; }

    /** Skia's number of the mode. */
    public int skia() { return skia; }

    static BlendMode fromPorterDuff(PorterDuff.Mode m) {
        switch (m) {
            case CLEAR: return CLEAR;
            case SRC: return SRC;
            case DST: return DST;
            case SRC_OVER: return SRC_OVER;
            case DST_OVER: return DST_OVER;
            case SRC_IN: return SRC_IN;
            case DST_IN: return DST_IN;
            case SRC_OUT: return SRC_OUT;
            case DST_OUT: return DST_OUT;
            case SRC_ATOP: return SRC_ATOP;
            case DST_ATOP: return DST_ATOP;
            case XOR: return XOR;
            case DARKEN: return DARKEN;
            case LIGHTEN: return LIGHTEN;
            case MULTIPLY: return MODULATE;
            case SCREEN: return SCREEN;
            case ADD: return PLUS;
            case OVERLAY: return OVERLAY;
            default: return SRC_OVER;
        }
    }
}
