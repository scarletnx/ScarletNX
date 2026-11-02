// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.app.AppComponentFactory;
import android.app.Application;
import android.content.res.Resources;

/** The process's app: its class loader, component factory, resources and Application, which Main sets at boot. */
public final class App {
    private static ClassLoader loader;
    private static AppComponentFactory factory = AppComponentFactory.DEFAULT;
    private static Resources resources;
    private static Application application;

    private App() {}

    public static void set(ClassLoader l, AppComponentFactory f, Resources r) {
        loader = l;
        factory = f;
        resources = r;
    }
    public static void setApplication(Application a) { application = a; }

    public static ClassLoader classLoader() { return loader; }
    public static AppComponentFactory factory() { return factory; }
    public static Resources resources() { return resources; }
    public static Application application() { return application; }

    /** A new base context for a component, with the theme it starts with. */
    static ContextImpl context(int theme) {
        ContextImpl c = new ContextImpl(resources, false);
        if (theme != 0) c.setTheme(theme);
        return c;
    }
}
