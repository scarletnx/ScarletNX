// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.print;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.Objects;

/** The attributes of a print: media size, resolution, margins, color and duplex modes. */
public final class PrintAttributes implements Parcelable {
    public static final int COLOR_MODE_MONOCHROME = 1, COLOR_MODE_COLOR = 2;
    public static final int DUPLEX_MODE_NONE = 1, DUPLEX_MODE_LONG_EDGE = 2, DUPLEX_MODE_SHORT_EDGE = 4;
    private MediaSize mediaSize;
    private Resolution resolution;
    private Margins minMargins;
    private int colorMode, duplexMode;

    PrintAttributes() {}
    public MediaSize getMediaSize() { return mediaSize; }
    public Resolution getResolution() { return resolution; }
    public Margins getMinMargins() { return minMargins; }
    public int getColorMode() { return colorMode; }
    public int getDuplexMode() { return duplexMode; }

    @Override public boolean equals(Object o) {
        return o instanceof PrintAttributes a && a.colorMode == colorMode && a.duplexMode == duplexMode
            && Objects.equals(a.mediaSize, mediaSize) && Objects.equals(a.resolution, resolution) && Objects.equals(a.minMargins, minMargins);
    }
    @Override public int hashCode() { return Objects.hash(mediaSize, resolution, minMargins, colorMode, duplexMode); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<PrintAttributes> CREATOR = new Parcelable.Creator<>() {
        @Override public PrintAttributes createFromParcel(Parcel in) { return (PrintAttributes) in.readValue(null); }
        @Override public PrintAttributes[] newArray(int size) { return new PrintAttributes[size]; }
    };

    /** A size of paper, in mils (thousandths of an inch). */
    public static final class MediaSize {
        public static final MediaSize UNKNOWN_PORTRAIT = new MediaSize("UNKNOWN_PORTRAIT", "android", 1, Integer.MAX_VALUE);
        public static final MediaSize UNKNOWN_LANDSCAPE = new MediaSize("UNKNOWN_LANDSCAPE", "android", Integer.MAX_VALUE, 1);
        private final String id, label;
        private final int widthMils, heightMils;

        public MediaSize(String id, String label, int widthMils, int heightMils) {
            if (widthMils <= 0) throw new IllegalArgumentException("widthMils cannot be less than or equal to zero.");
            if (heightMils <= 0) throw new IllegalArgumentException("heightMils cannot be less than or equal to zero.");
            this.id = id; this.label = label; this.widthMils = widthMils; this.heightMils = heightMils;
        }
        public String getId() { return id; }
        public String getLabel(android.content.pm.PackageManager packageManager) { return label; }
        public int getWidthMils() { return widthMils; }
        public int getHeightMils() { return heightMils; }
        public boolean isPortrait() { return heightMils >= widthMils; }
        public MediaSize asPortrait() { return isPortrait() ? this : new MediaSize(id, label, heightMils, widthMils); }
        public MediaSize asLandscape() { return isPortrait() ? new MediaSize(id, label, heightMils, widthMils) : this; }
        @Override public boolean equals(Object o) { return o instanceof MediaSize m && m.widthMils == widthMils && m.heightMils == heightMils; }
        @Override public int hashCode() { return widthMils * 31 + heightMils; }
    }

    /** A resolution, in dots per inch. */
    public static final class Resolution {
        private final String id, label;
        private final int horizontalDpi, verticalDpi;

        public Resolution(String id, String label, int horizontalDpi, int verticalDpi) {
            this.id = id; this.label = label; this.horizontalDpi = horizontalDpi; this.verticalDpi = verticalDpi;
        }
        public String getId() { return id; }
        public String getLabel() { return label; }
        public int getHorizontalDpi() { return horizontalDpi; }
        public int getVerticalDpi() { return verticalDpi; }
        @Override public boolean equals(Object o) { return o instanceof Resolution r && r.horizontalDpi == horizontalDpi && r.verticalDpi == verticalDpi; }
        @Override public int hashCode() { return horizontalDpi * 31 + verticalDpi; }
    }

    /** Margins, in mils. */
    public static final class Margins {
        public static final Margins NO_MARGINS = new Margins(0, 0, 0, 0);
        private final int left, top, right, bottom;

        public Margins(int leftMils, int topMils, int rightMils, int bottomMils) { left = leftMils; top = topMils; right = rightMils; bottom = bottomMils; }
        public int getLeftMils() { return left; }
        public int getTopMils() { return top; }
        public int getRightMils() { return right; }
        public int getBottomMils() { return bottom; }
        @Override public boolean equals(Object o) { return o instanceof Margins m && m.left == left && m.top == top && m.right == right && m.bottom == bottom; }
        @Override public int hashCode() { return Objects.hash(left, top, right, bottom); }
    }

    public static final class Builder {
        private final PrintAttributes attributes = new PrintAttributes();
        public Builder() {}
        public Builder setMediaSize(MediaSize mediaSize) { attributes.mediaSize = mediaSize; return this; }
        public Builder setResolution(Resolution resolution) { attributes.resolution = resolution; return this; }
        public Builder setMinMargins(Margins margins) { attributes.minMargins = margins; return this; }
        public Builder setColorMode(int colorMode) { attributes.colorMode = colorMode; return this; }
        public Builder setDuplexMode(int duplexMode) { attributes.duplexMode = duplexMode; return this; }
        public PrintAttributes build() {
            PrintAttributes a = new PrintAttributes();
            a.mediaSize = attributes.mediaSize; a.resolution = attributes.resolution; a.minMargins = attributes.minMargins;
            a.colorMode = attributes.colorMode; a.duplexMode = attributes.duplexMode;
            return a;
        }
    }
}
