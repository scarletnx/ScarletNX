// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** The standard direction heuristics: the first strong character's, or any right to left one's. */
public class TextDirectionHeuristics {
    private static boolean rtl(char c) {
        byte d = Character.getDirectionality(c);
        return d == Character.DIRECTIONALITY_RIGHT_TO_LEFT || d == Character.DIRECTIONALITY_RIGHT_TO_LEFT_ARABIC;
    }

    private static boolean ltr(char c) { return Character.getDirectionality(c) == Character.DIRECTIONALITY_LEFT_TO_RIGHT; }

    private record Heuristic(int kind, boolean fallback) implements TextDirectionHeuristic {
        @Override public boolean isRtl(char[] array, int start, int count) { return isRtl(new String(array, start, count), 0, count); }

        @Override public boolean isRtl(CharSequence cs, int start, int count) {
            switch (kind) {
                case 0: return false;
                case 1: return true;
                case 2:
                    for (int i = start; i < start + count; ++i) if (rtl(cs.charAt(i))) return true;
                    return fallback;
                default:
                    for (int i = start; i < start + count; ++i) {
                        char c = cs.charAt(i);
                        if (rtl(c)) return true;
                        if (ltr(c)) return false;
                    }
                    return fallback;
            }
        }
    }

    public static final TextDirectionHeuristic LTR = new Heuristic(0, false);
    public static final TextDirectionHeuristic RTL = new Heuristic(1, true);
    public static final TextDirectionHeuristic FIRSTSTRONG_LTR = new Heuristic(3, false);
    public static final TextDirectionHeuristic FIRSTSTRONG_RTL = new Heuristic(3, true);
    public static final TextDirectionHeuristic ANYRTL_LTR = new Heuristic(2, false);
    public static final TextDirectionHeuristic LOCALE = FIRSTSTRONG_LTR;
}
