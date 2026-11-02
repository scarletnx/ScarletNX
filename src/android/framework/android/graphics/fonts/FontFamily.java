// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.fonts;

import java.util.ArrayList;

/** Faces of one family. */
public final class FontFamily {
    private final ArrayList<Font> fonts;

    private FontFamily(ArrayList<Font> fonts) { this.fonts = fonts; }
    public Font getFont(int index) { return index < fonts.size() ? fonts.get(index) : null; }
    public int getSize() { return fonts.size(); }

    public static final class Builder {
        private final ArrayList<Font> fonts = new ArrayList<>();

        public Builder(Font font) { fonts.add(font); }
        public Builder addFont(Font font) { fonts.add(font); return this; }
        public FontFamily build() { return new FontFamily(fonts); }
    }
}
