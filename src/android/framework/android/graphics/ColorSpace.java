// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** Color spaces: the named ones as sRGB-family RGB spaces; colors are drawn in sRGB. */
public abstract class ColorSpace {
    public enum Named {
        SRGB, LINEAR_SRGB, EXTENDED_SRGB, LINEAR_EXTENDED_SRGB, BT709, BT2020, DCI_P3, DISPLAY_P3, NTSC_1953, SMPTE_C,
        ADOBE_RGB, PRO_PHOTO_RGB, ACES, ACESCG, CIE_XYZ, CIE_LAB, BT2020_HLG, BT2020_PQ, OK_LAB
    }

    public enum Model { RGB(3), XYZ(3), LAB(3), CMYK(4);
        private final int count;
        Model(int count) { this.count = count; }
        public int getComponentCount() { return count; }
    }

    public enum RenderIntent { PERCEPTUAL, RELATIVE, SATURATION, ABSOLUTE }

    public static final float[] ILLUMINANT_D50 = {0.345704f, 0.358540f};
    public static final float[] ILLUMINANT_D65 = {0.31271f, 0.32902f};
    private static final Rgb[] NAMED = new Rgb[Named.values().length];
    private final String name;
    private final int id;

    ColorSpace(String name, int id) { this.name = name; this.id = id; }

    public static ColorSpace get(Named name) {
        synchronized (NAMED) {
            int i = name.ordinal();
            if (NAMED[i] == null) NAMED[i] = new Rgb(name.name(), i, new float[] {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f}, ILLUMINANT_D65,
                new Rgb.TransferParameters(1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045, 2.4));
            return NAMED[i];
        }
    }

    public static ColorSpace match(float[] toXYZD50, Rgb.TransferParameters function) { return get(Named.SRGB); }
    public static ColorSpace adapt(ColorSpace colorSpace, float[] whitePoint) { return colorSpace; }

    public String getName() { return name; }
    public int getId() { return id; }
    public Model getModel() { return Model.RGB; }
    public int getComponentCount() { return 3; }
    public boolean isWideGamut() { return false; }
    public boolean isSrgb() { return id == 0; }
    public abstract float getMinValue(int component);
    public abstract float getMaxValue(int component);
    public float[] toXyz(float r, float g, float b) { return new float[] {r, g, b}; }
    public abstract float[] toXyz(float[] v);
    public float[] fromXyz(float x, float y, float z) { return new float[] {x, y, z}; }
    public abstract float[] fromXyz(float[] v);
    @Override public String toString() { return name + " (id=" + id + ")"; }

    public static class Rgb extends ColorSpace {
        public static class TransferParameters {
            public final double a, b, c, d, e, f, g;
            public TransferParameters(double a, double b, double c, double d, double g) { this(a, b, c, d, 0, 0, g); }
            public TransferParameters(double a, double b, double c, double d, double e, double f, double g) {
                this.a = a; this.b = b; this.c = c; this.d = d; this.e = e; this.f = f; this.g = g;
            }
        }

        private final float[] primaries, whitePoint;
        private final TransferParameters transfer;

        Rgb(String name, int id, float[] primaries, float[] whitePoint, TransferParameters transfer) {
            super(name, id);
            this.primaries = primaries;
            this.whitePoint = whitePoint;
            this.transfer = transfer;
        }

        public Rgb(String name, float[] primaries, float[] whitePoint, TransferParameters function) { this(name, -1, primaries, whitePoint, function); }
        public Rgb(String name, float[] toXYZ, TransferParameters function) { this(name, -1, new float[6], ILLUMINANT_D65, function); }
        public Rgb(String name, float[] primaries, float[] whitePoint, java.util.function.DoubleUnaryOperator oetf,
                   java.util.function.DoubleUnaryOperator eotf, float min, float max) { this(name, -1, primaries, whitePoint, null); }

        public float[] getPrimaries() { return primaries.clone(); }
        public float[] getWhitePoint() { return whitePoint.clone(); }
        public TransferParameters getTransferParameters() { return transfer; }
        /** The opto-electronic transfer: sRGB's, or the parametric function's inverse */
        public java.util.function.DoubleUnaryOperator getOetf() {
            TransferParameters t = transfer;
            if (t == null) return x -> x <= 0.0031308 ? x * 12.92 : 1.055 * Math.pow(x, 1 / 2.4) - 0.055;
            return x -> x >= t.d * t.c ? (Math.pow(x - t.e, 1 / t.g) - t.b) / t.a : (x - t.f) / t.c;
        }
        /** The electro-optical transfer: sRGB's, or the parametric function */
        public java.util.function.DoubleUnaryOperator getEotf() {
            TransferParameters t = transfer;
            if (t == null) return x -> x <= 0.04045 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4);
            return x -> x >= t.d ? Math.pow(t.a * x + t.b, t.g) + t.e : t.c * x + t.f;
        }
        public float[] getTransform() { return new float[] {0.4124f, 0.2126f, 0.0193f, 0.3576f, 0.7152f, 0.1192f, 0.1805f, 0.0722f, 0.9505f}; }
        public float[] getInverseTransform() { return new float[] {3.2406f, -0.9689f, 0.0557f, -1.5372f, 1.8758f, -0.2040f, -0.4986f, 0.0415f, 1.0570f}; }
        @Override public float getMinValue(int component) { return 0; }
        @Override public float getMaxValue(int component) { return 1; }
        @Override public float[] toXyz(float[] v) { return v; }
        @Override public float[] fromXyz(float[] v) { return v; }
    }
}
