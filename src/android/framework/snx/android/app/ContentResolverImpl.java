// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.content.ContentProvider;
import android.content.ContentResolver;

/** The content resolver of the process: the app's own providers by authority, the only ones on this device. */
final class ContentResolverImpl extends ContentResolver {
    private static ContentResolverImpl instance;

    private ContentResolverImpl() { super(App.application()); }

    static synchronized ContentResolverImpl get() {
        if (instance == null) instance = new ContentResolverImpl();
        return instance;
    }

    @Override protected ContentProvider provider(String authority) { return Providers.get(authority); }
}
