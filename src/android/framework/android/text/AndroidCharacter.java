// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Character properties that java.lang.Character does not give. */
public class AndroidCharacter {
    public static final int EAST_ASIAN_WIDTH_NEUTRAL = 0, EAST_ASIAN_WIDTH_AMBIGUOUS = 1, EAST_ASIAN_WIDTH_HALF_WIDTH = 2,
        EAST_ASIAN_WIDTH_FULL_WIDTH = 3, EAST_ASIAN_WIDTH_NARROW = 4, EAST_ASIAN_WIDTH_WIDE = 5;
    /** Pairs of characters that are mirror glyphs of each other. */
    private static final String MIRRORS = "()<>[]{}«»‹›⁅⁆⁽⁾₍₎≤≥"
        + "∈∋⊂⊃⊆⊇〈〉⟦⟧⟨⟩⟪⟫〈〉《》"
        + "「」『』【】〔〕〖〗〘〙〚〛（）＜＞"
        + "［］｛｝｟｠｢｣";

    public AndroidCharacter() {}

    /** The mirror glyph of ch, or ch when it has none. */
    public static char getMirror(char ch) {
        int i = MIRRORS.indexOf(ch);
        return i < 0 ? ch : MIRRORS.charAt(i ^ 1);
    }
    /** Mirrors the characters that have mirrors, and returns true if it changed one. */
    public static boolean mirror(char[] text, int start, int count) {
        boolean changed = false;
        for (int i = start; i < start + count; ++i) {
            char m = getMirror(text[i]);
            if (m != text[i]) { text[i] = m; changed = true; }
        }
        return changed;
    }
}
