// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.content.ContentProvider;
import android.content.pm.ProviderInfo;
import java.util.ArrayList;
import java.util.HashMap;

/** The app's content providers: made at boot, before Application.onCreate, as ActivityThread.installContentProviders. */
public final class Providers {
    private static final HashMap<String, ContentProvider> byAuthority = new HashMap<>();

    private Providers() {}

    public static void install() {
        ArrayList<ProviderInfo> providers = new ArrayList<>(Package.get().providers);
        providers.sort((a, b) -> Integer.compare(b.initOrder, a.initOrder));
        for (ProviderInfo info : providers) {
            if (!info.enabled && Package.get().enabledState(new android.content.ComponentName(info.packageName, info.name)) != 1) continue;
            ContentProvider p;
            try {
                p = App.factory().instantiateProvider(App.classLoader(), info.name);
            } catch (ReflectiveOperationException e) {
                throw new RuntimeException("Unable to get provider " + info.name, e);
            }
            synchronized (byAuthority) {
                if (info.authority != null) for (String a : info.authority.split(";")) byAuthority.put(a, p);
            }
            p.attachInfo(App.application(), info);
        }
    }

    /** the system's providers that an app reaches (the shared storage's documents), made when first asked for */
    private static ContentProvider system(String authority) {
        if (!snx.android.provider.ExternalStorageProvider.AUTHORITY.equals(authority)) return null;
        android.content.pm.ProviderInfo info = new android.content.pm.ProviderInfo();
        info.authority = authority;
        info.packageName = "com.android.externalstorage";
        info.name = snx.android.provider.ExternalStorageProvider.class.getName();
        info.exported = true;
        info.grantUriPermissions = true;
        ContentProvider p = new snx.android.provider.ExternalStorageProvider();
        p.attachInfo(App.application(), info);
        byAuthority.put(authority, p);
        return p;
    }

    static ContentProvider get(String authority) {
        synchronized (byAuthority) {
            ContentProvider p = byAuthority.get(authority);
            return p != null ? p : system(authority);
        }
    }
}
