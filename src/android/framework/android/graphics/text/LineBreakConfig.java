// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.text;

/** How lines break: the styles of line and word breaking. */
public final class LineBreakConfig {
    public static final int LINE_BREAK_STYLE_NONE = 0, LINE_BREAK_STYLE_LOOSE = 1, LINE_BREAK_STYLE_NORMAL = 2,
        LINE_BREAK_STYLE_STRICT = 3, LINE_BREAK_STYLE_AUTO = 5, LINE_BREAK_STYLE_UNSPECIFIED = -1;
    public static final int LINE_BREAK_WORD_STYLE_NONE = 0, LINE_BREAK_WORD_STYLE_PHRASE = 1,
        LINE_BREAK_WORD_STYLE_AUTO = 2, LINE_BREAK_WORD_STYLE_UNSPECIFIED = -1;
    public static final int HYPHENATION_UNSPECIFIED = -1, HYPHENATION_DISABLED = 0, HYPHENATION_ENABLED = 1;
    public static final LineBreakConfig NONE = new LineBreakConfig(LINE_BREAK_STYLE_NONE, LINE_BREAK_WORD_STYLE_NONE);
    private final int style, wordStyle;

    private LineBreakConfig(int style, int wordStyle) { this.style = style; this.wordStyle = wordStyle; }
    public int getLineBreakStyle() { return style; }
    public int getLineBreakWordStyle() { return wordStyle; }
    public int getHyphenation() { return HYPHENATION_UNSPECIFIED; }
    public LineBreakConfig merge(LineBreakConfig config) { return config != null ? config : this; }
    public static LineBreakConfig getLineBreakConfig(int lineBreakStyle, int lineBreakWordStyle) { return new LineBreakConfig(lineBreakStyle, lineBreakWordStyle); }

    public static final class Builder {
        private int style = LINE_BREAK_STYLE_UNSPECIFIED, wordStyle = LINE_BREAK_WORD_STYLE_UNSPECIFIED;

        public Builder() {}
        public Builder merge(LineBreakConfig config) { style = config.style; wordStyle = config.wordStyle; return this; }
        public Builder setLineBreakStyle(int lineBreakStyle) { style = lineBreakStyle; return this; }
        public Builder setLineBreakWordStyle(int lineBreakWordStyle) { wordStyle = lineBreakWordStyle; return this; }
        public Builder setHyphenation(int hyphenation) { return this; }
        public LineBreakConfig build() { return new LineBreakConfig(style, wordStyle); }
    }
}
