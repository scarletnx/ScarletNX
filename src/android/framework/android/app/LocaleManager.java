// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.os.LocaleList;

/** The locales of the system and of the app: the host's locale, and the app's own choice, which lasts while it runs. */
public class LocaleManager {
    private LocaleList applicationLocales = LocaleList.getEmptyLocaleList();

    public LocaleList getSystemLocales() { return LocaleList.getDefault(); }
    public LocaleList getApplicationLocales() { return applicationLocales; }
    public LocaleList getApplicationLocales(String appPackageName) { return applicationLocales; }
    public void setApplicationLocales(LocaleList locales) { applicationLocales = locales; }
}
