// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.preference;

import android.content.Context;
import android.content.SharedPreferences;

/** The preferences of a preference hierarchy. Only the default preferences file is here. */
@Deprecated
public class PreferenceManager {
    private PreferenceManager() {}
    public static String getDefaultSharedPreferencesName(Context context) { return context.getPackageName() + "_preferences"; }
    public static SharedPreferences getDefaultSharedPreferences(Context context) {
        return context.getSharedPreferences(getDefaultSharedPreferencesName(context), Context.MODE_PRIVATE);
    }
}
