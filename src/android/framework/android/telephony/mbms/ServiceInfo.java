// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.telephony.mbms;

import java.util.Locale;
import java.util.Map;
import java.util.NoSuchElementException;
import java.util.Set;

/** A broadcast service of the carrier. There is no telephony, so an app never gets one. */
public class ServiceInfo {
    private final Map<Locale, String> names;

    ServiceInfo(Map<Locale, String> names) { this.names = Map.copyOf(names); }
    public Set<Locale> getNamedContentLocales() { return names.keySet(); }
    public CharSequence getNameForLocale(Locale locale) {
        String name = names.get(locale);
        if (name == null) throw new NoSuchElementException("Locale not supported");
        return name;
    }
}
