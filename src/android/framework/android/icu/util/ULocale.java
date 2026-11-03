// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.icu.util;

import java.util.Locale;
import java.util.Map;

/** A locale, over java.util.Locale. Likely subtags come from a table of the common languages. */
public final class ULocale implements java.io.Serializable, Comparable<ULocale> {
    private static final long serialVersionUID = 1L;
    /** The likely script and region of a language. */
    private static final Map<String, String[]> LIKELY = Map.ofEntries(
        Map.entry("ar", new String[] {"Arab", "EG"}), Map.entry("bn", new String[] {"Beng", "BD"}),
        Map.entry("de", new String[] {"Latn", "DE"}), Map.entry("el", new String[] {"Grek", "GR"}),
        Map.entry("en", new String[] {"Latn", "US"}), Map.entry("es", new String[] {"Latn", "ES"}),
        Map.entry("fa", new String[] {"Arab", "IR"}), Map.entry("fr", new String[] {"Latn", "FR"}),
        Map.entry("he", new String[] {"Hebr", "IL"}), Map.entry("hi", new String[] {"Deva", "IN"}),
        Map.entry("id", new String[] {"Latn", "ID"}), Map.entry("it", new String[] {"Latn", "IT"}),
        Map.entry("ja", new String[] {"Jpan", "JP"}), Map.entry("ko", new String[] {"Kore", "KR"}),
        Map.entry("nl", new String[] {"Latn", "NL"}), Map.entry("pl", new String[] {"Latn", "PL"}),
        Map.entry("pt", new String[] {"Latn", "BR"}), Map.entry("ru", new String[] {"Cyrl", "RU"}),
        Map.entry("sv", new String[] {"Latn", "SE"}), Map.entry("th", new String[] {"Thai", "TH"}),
        Map.entry("tr", new String[] {"Latn", "TR"}), Map.entry("uk", new String[] {"Cyrl", "UA"}),
        Map.entry("vi", new String[] {"Latn", "VN"}), Map.entry("zh", new String[] {"Hans", "CN"}));
    public static final ULocale ROOT = new ULocale(Locale.ROOT), ENGLISH = new ULocale(Locale.ENGLISH),
        US = new ULocale(Locale.US), JAPANESE = new ULocale(Locale.JAPANESE), CHINESE = new ULocale(Locale.CHINESE);
    private final Locale locale;

    private ULocale(Locale locale) { this.locale = locale; }
    public ULocale(String localeID) { this(Locale.forLanguageTag(localeID.replace('_', '-'))); }
    public static ULocale forLocale(Locale loc) { return loc == null ? null : new ULocale(loc); }
    public static ULocale forLanguageTag(String languageTag) { return new ULocale(Locale.forLanguageTag(languageTag)); }
    public static ULocale getDefault() { return new ULocale(Locale.getDefault()); }
    public Locale toLocale() { return locale; }
    public String getLanguage() { return locale.getLanguage(); }
    public String getScript() { return locale.getScript(); }
    public String getCountry() { return locale.getCountry(); }
    public String getVariant() { return locale.getVariant(); }
    public String toLanguageTag() { return locale.toLanguageTag(); }

    public static ULocale addLikelySubtags(ULocale loc) {
        String lang = loc.getLanguage(), script = loc.getScript(), region = loc.getCountry();
        String[] likely = LIKELY.get(lang.isEmpty() ? "en" : lang);
        if (likely == null) return loc;
        if (lang.equals("zh") && script.isEmpty() && (region.equals("TW") || region.equals("HK") || region.equals("MO"))) script = "Hant";
        Locale.Builder b = new Locale.Builder().setLocale(loc.locale);
        if (lang.isEmpty()) b.setLanguage("en");
        if (script.isEmpty()) b.setScript(likely[0]);
        if (region.isEmpty()) b.setRegion(lang.equals("zh") && "Hant".equals(script) ? "TW" : likely[1]);
        return new ULocale(b.build());
    }

    @Override public boolean equals(Object o) { return o instanceof ULocale u && locale.equals(u.locale); }
    @Override public int hashCode() { return locale.hashCode(); }
    @Override public String toString() { return locale.toString(); }
    @Override public int compareTo(ULocale other) { return toString().compareTo(other.toString()); }
}
