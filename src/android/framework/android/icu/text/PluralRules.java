// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// RULES holds Unicode CLDR's cardinal plural rules (common/supplemental/plurals.xml, version 47):
// Copyright (C) 1991-2025 Unicode, Inc.
// SPDX-License-Identifier: Unicode-3.0
package android.icu.text;

import android.icu.util.ULocale;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Set;

/**
 * A language's plural categories for a number (cardinals), as ICU selects them: the rules of Unicode CLDR, which
 * Android 16's ICU and the JDK both carry. A rule is relations joined by "and", then "or"; a relation is an operand of
 * the number, maybe modulo a value, that is (=) or is not (!=) one of a list of values and ranges (of integers only).
 */
public class PluralRules implements java.io.Serializable {
    private static final long serialVersionUID = 1L;
    public static final String KEYWORD_ZERO = "zero", KEYWORD_ONE = "one", KEYWORD_TWO = "two", KEYWORD_FEW = "few",
        KEYWORD_MANY = "many", KEYWORD_OTHER = "other";
    /** Other for every number, the rules of a language with no categories */
    public static final PluralRules DEFAULT = new PluralRules("");

    /**
     * CLDR's cardinal rules, from its plurals.xml (Unicode License v3), each with the languages that follow it (a
     * region where it differs from its language's: ICU takes any other region's from its language); a language not
     * listed has other alone. The categories go in CLDR's order, the order ICU tries them in. Android 16's ICU gives
     * the same category for each language here, for 0 to 230, 1000, 10^5, 10^6, 2 x 10^6 and ten fractions.
     */
    private static final String[][] RULES = {
        {"one: i = 0 or n = 1",
         "am as bn doi fa gu hi kn pcm zu"},
        {"one: i = 0 or n = 1;few: n = 2..10",
         "shi"},
        {"one: i = 0,1",
         "ff hy kab"},
        {"one: i = 0,1;many: e = 0 and i != 0 and i % 1000000 = 0 and v = 0 or e != 0..5",
         "fr"},
        {"one: i = 0..1;many: e = 0 and i != 0 and i % 1000000 = 0 and v = 0 or e != 0..5",
         "pt"},
        {"one: i = 1 and v = 0",
         "ast de en et fi fy gl ia lij nl sc sv sw ur yi"},
        {"one: i = 1 and v = 0 or i = 0 and v != 0;two: i = 2 and v = 0",
         "he"},
        {"one: i = 1 and v = 0;few: i = 2..4 and v = 0;many: v != 0",
         "cs sk"},
        {"one: i = 1 and v = 0;few: v != 0 or n = 0 or n != 1 and n % 100 = 1..19",
         "ro"},
        {"one: i = 1 and v = 0;few: v = 0 and i % 10 = 2..4 and i % 100 != 12..14;many: v = 0 and i != 1 and i % " +
         "10 = 0..1 or v = 0 and i % 10 = 5..9 or v = 0 and i % 100 = 12..14",
         "pl"},
        {"one: i = 1 and v = 0;many: e = 0 and i != 0 and i % 1000000 = 0 and v = 0 or e != 0..5",
         "ca it pt_PT vec"},
        {"one: n % 10 = 1 and n % 100 != 11,71,91;two: n % 10 = 2 and n % 100 != 12,72,92;few: n % 10 = 3..4,9 and " +
         "n % 100 != 10..19,70..79,90..99;many: n != 0 and n % 1000000 = 0",
         "br"},
        {"one: n % 10 = 1 and n % 100 != 11..19;few: n % 10 = 2..9 and n % 100 != 11..19;many: f != 0",
         "lt"},
        {"one: n % 10 = 1 and n % 100 != 11;few: n % 10 = 2..4 and n % 100 != 12..14;many: n % 10 = 0 or n % 10 = " +
         "5..9 or n % 100 = 11..14",
         "be"},
        {"one: n = 0,1 or i = 0 and f = 1",
         "si"},
        {"one: n = 0..1",
         "ak bho csw ln mg nso pa ti"},
        {"one: n = 0..1 or n = 11..99",
         "tzm"},
        {"one: n = 1",
         "af asa az bem bez bg brx ce cgg chr ckb ee el eo eu fo fur gsw ha haw hu jgo jmc ka kk kkj kl ks ksb ku " +
         "ky lb lg mas mgo ml mn mr nb nd ne nn nnh no nyn om or os ps rm rof rwk saq sd seh sn so sq st syr ta te " +
         "teo tk tn tr ug uz vun wae xh xog"},
        {"one: n = 1 or t != 0 and i = 0,1",
         "da"},
        {"one: n = 1,11;two: n = 2,12;few: n = 3..10,13..19",
         "gd"},
        {"one: n = 1;two: n = 2;few: n = 0 or n % 100 = 3..10;many: n % 100 = 11..19",
         "mt"},
        {"one: n = 1;two: n = 2;few: n = 3..6;many: n = 7..10",
         "ga"},
        {"one: n = 1;many: e = 0 and i != 0 and i % 1000000 = 0 and v = 0 or e != 0..5",
         "es"},
        {"one: n = 1;two: n = 2",
         "naq sat se smn sms"},
        {"one: t = 0 and i % 10 = 1 and i % 100 != 11 or t % 10 = 1 and t % 100 != 11",
         "is"},
        {"one: v = 0 and i % 10 = 1 and i % 100 != 11 or f % 10 = 1 and f % 100 != 11",
         "mk"},
        {"one: v = 0 and i % 10 = 1 and i % 100 != 11 or f % 10 = 1 and f % 100 != 11;few: v = 0 and i % 10 = 2..4 " +
         "and i % 100 != 12..14 or f % 10 = 2..4 and f % 100 != 12..14",
         "bs hr sr"},
        {"one: v = 0 and i % 10 = 1 and i % 100 != 11;few: v = 0 and i % 10 = 2..4 and i % 100 != 12..14;many: v = " +
         "0 and i % 10 = 0 or v = 0 and i % 10 = 5..9 or v = 0 and i % 100 = 11..14",
         "ru uk"},
        {"one: v = 0 and i % 10 = 1;two: v = 0 and i % 10 = 2;few: v = 0 and i % 100 = 0,20,40,60,80;many: v != 0",
         "gv"},
        {"one: v = 0 and i % 100 = 1 or f % 100 = 1;two: v = 0 and i % 100 = 2 or f % 100 = 2;few: v = 0 and i % " +
         "100 = 3..4 or f % 100 = 3..4",
         "dsb hsb"},
        {"one: v = 0 and i % 100 = 1;two: v = 0 and i % 100 = 2;few: v = 0 and i % 100 = 3..4 or v != 0",
         "sl"},
        {"one: v = 0 and i = 1,2,3 or v = 0 and i % 10 != 4,6,9 or v != 0 and f % 10 != 4,6,9",
         "ceb fil"},
        {"zero: n % 10 = 0 or n % 100 = 11..19 or v = 2 and f % 100 = 11..19;one: n % 10 = 1 and n % 100 != 11 or " +
         "v = 2 and f % 10 = 1 and f % 100 != 11 or v != 2 and f % 10 = 1",
         "lv"},
        {"zero: n = 0;one: i = 0,1 and n != 0",
         "lag"},
        {"zero: n = 0;one: n = 1",
         "blo ksh"},
        {"zero: n = 0;one: n = 1;two: n % 100 = 2,22,42,62,82 or n % 1000 = 0 and n % 100000 = " +
         "1000..20000,40000,60000,80000 or n != 0 and n % 1000000 = 100000;few: n % 100 = 3,23,43,63,83;many: n != " +
         "1 and n % 100 = 1,21,41,61,81",
         "kw"},
        {"zero: n = 0;one: n = 1;two: n = 2;few: n % 100 = 3..10;many: n % 100 = 11..99",
         "ar"},
        {"zero: n = 0;one: n = 1;two: n = 2;few: n = 3;many: n = 6",
         "cy"}
    };

    private final String description;
    /** For each category but other: its keyword, then its rule's "or" terms, each an array of relations */
    private final String[] keywords;
    private final String[][][] conditions;

    private PluralRules(String description) {
        this.description = description;
        String[] rules = description.isEmpty() ? new String[0] : description.split(";");
        keywords = new String[rules.length];
        conditions = new String[rules.length][][];
        for (int k = 0; k < rules.length; ++k) {
            int colon = rules[k].indexOf(':');
            keywords[k] = rules[k].substring(0, colon).trim();
            String[] or = rules[k].substring(colon + 1).split(" or ");
            conditions[k] = new String[or.length][];
            for (int o = 0; o < or.length; ++o) conditions[k][o] = or[o].trim().split(" and ");
        }
    }

    /** The rules of a locale's language, or of its region where CLDR gives that one rules of its own */
    public static PluralRules forLocale(Locale locale) {
        String language = locale.getLanguage(), region = language + "_" + locale.getCountry();
        String found = null;
        for (String[] r : RULES) {
            for (String l : r[1].split(" ")) {
                if (l.equals(region)) return new PluralRules(r[0]);
                if (l.equals(language)) found = r[0];
            }
        }
        return found != null ? new PluralRules(found) : DEFAULT;
    }

    public static PluralRules forLocale(ULocale locale) { return forLocale(locale.toLocale()); }

    /** The category of a number: the first whose rule it meets, else other */
    public String select(double number) {
        double[] operands = operands(number);
        for (int k = 0; k < keywords.length; ++k) {
            for (String[] and : conditions[k]) {
                boolean all = true;
                for (String relation : and) all = all && holds(relation, operands);
                if (all) return keywords[k];
            }
        }
        return KEYWORD_OTHER;
    }

    public Set<String> getKeywords() {
        Set<String> all = new LinkedHashSet<>();
        Collections.addAll(all, keywords);
        all.add(KEYWORD_OTHER);
        return all;
    }

    @Override public String toString() { return description; }
    @Override public boolean equals(Object o) {
        return o instanceof PluralRules p && p.description.equals(description);
    }
    @Override public int hashCode() { return description.hashCode(); }

    /**
     * CLDR's operands n, i, v, w, f, t of a number, as ICU makes them of a double: its absolute value, its integer
     * digits, the count of its visible fraction digits, that count without trailing zeros, and those digits with and
     * without trailing zeros. ICU counts the fraction digits of a number below 1e9 in its first 6 (truncated: 2.01 is
     * 2.0099999, 6 digits), and of a larger one in its 15-digit scientific form. e and c, the exponent of a compact
     * form, are 0
     */
    private static double[] operands(double number) {
        double n = Math.abs(number), i = Math.floor(n);
        int v = 0;
        if (n != i && !Double.isInfinite(n) && !Double.isNaN(n)) {
            if (n < 1e9) {
                long digits = (long) (n * 1000000) % 1000000;
                for (int mask = 10, count = 6; count > 0 && v == 0; mask *= 10, --count)
                    if (digits % mask != 0) v = count;
            } else {
                String sci = String.format(Locale.ENGLISH, "%1.15e", n);
                int e = sci.lastIndexOf('e');
                v = Math.max(0, e - 2 - Integer.parseInt(sci.substring(e + 1).replace("+", "")));
                for (int k = e - 1; v > 0 && sci.charAt(k) == '0'; --k) --v;
            }
        }
        long scale = (long) Math.pow(10, v), f = v == 0 ? 0 : Math.round(n * scale) % scale, t = f;
        int w = v;
        while (t != 0 && t % 10 == 0) {
            t /= 10;
            --w;
        }
        if (f == 0) w = 0;
        return new double[] {n, i, v, w, f, t};
    }

    /** Whether a relation ("i % 10 = 2..4,9" or "v != 0") holds for the operands */
    private static boolean holds(String relation, double[] operands) {
        boolean not = relation.contains("!=");
        int at = relation.indexOf(not ? "!=" : "=");
        String left = relation.substring(0, at).trim();
        double value = switch (left.charAt(0)) {
            case 'n' -> operands[0];
            case 'i' -> operands[1];
            case 'v' -> operands[2];
            case 'w' -> operands[3];
            case 'f' -> operands[4];
            case 't' -> operands[5];
            case 'e', 'c' -> 0;
            default -> throw new IllegalArgumentException("snx: plural rule operand " + left + " in " + relation);
        };
        int modulo = left.indexOf('%');
        if (modulo > 0) value %= Long.parseLong(left.substring(modulo + 1).trim());
        boolean in = false;
        for (String item : relation.substring(at + (not ? 2 : 1)).split(",")) {
            int dots = item.indexOf("..");
            if (dots < 0) {
                in |= value == Long.parseLong(item.trim());
            } else {
                in |= value == Math.floor(value) && value >= Long.parseLong(item.substring(0, dots).trim())
                    && value <= Long.parseLong(item.substring(dots + 2).trim());
            }
        }
        return in != not;
    }
}
