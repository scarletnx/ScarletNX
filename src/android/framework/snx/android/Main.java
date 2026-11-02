// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android;

import snx.android.app.Activities;
import snx.android.app.App;
import snx.android.app.ContextImpl;
import snx.android.app.Package;
import snx.android.app.Providers;
import snx.dalvik.AppClassLoader;

/** Where the runtime starts the app: the manifest read, the app's classes loaded and its components started. */
public final class Main {
    private Main() {}

    /** The app's process name: its package's. */
    public static String processName() {
        return Package.get().name();
    }

    /**
     * The app's folder on the host, where its data, cache and storage folders are: snx.android.data, which the
     * runtime sets, or android/PACKAGE in ScarletNX's data folder.
     */
    public static java.io.File dataRoot() {
        String named = System.getProperty("snx.android.data");
        java.io.File root = named != null ? new java.io.File(named)
            : new java.io.File(System.getProperty("user.home"), ".local/share/scarletnx/android/" + Package.get().name());
        root.mkdirs();
        return root;
    }

    /** A component's class name: one the manifest gives as ".Name" or "Name" is in the app's package. */
    public static String className(String pkg, String name) {
        if (name == null) return null;
        if (name.startsWith(".")) return pkg + name;
        return name.indexOf('.') < 0 ? pkg + "." + name : name;
    }

    private static int status;

    /** The process's end, as the last activity's finish brings it: the main loop stops, main returns status. */
    public static void exit(int code) {
        status = code;
        android.os.Looper.getMainLooper().quitSafely();
    }

    /** The exit status that main left, for the host. */
    public static int status() {
        return status;
    }

    /**
     * The boot, in the order of Android's ActivityThread.handleBindApplication and its launch of the first activity:
     * the main looper, the resources and the manifest, the component factory and the Application, the content
     * providers before Application.onCreate, then the launcher activity, and the main loop.
     */
    public static void main(String[] args) throws Throwable {
        // an exception that no thread catches ends the process, as RuntimeInit's handler does
        Thread.setDefaultUncaughtExceptionHandler((thread, e) -> {
            android.util.Log.e("AndroidRuntime", "FATAL EXCEPTION: " + thread.getName(), e);
            Runtime.getRuntime().halt(1);
        });
        try {
            boot();
        } catch (Throwable e) {
            Thread.getDefaultUncaughtExceptionHandler().uncaughtException(Thread.currentThread(), e);
        }
        // the app's threads end with the process, as on Android
        System.exit(status);
    }

    private static void boot() throws Throwable {
        android.os.Looper.prepareMainLooper();
        android.content.res.Resources res = android.content.res.Resources.getSystem();
        Package pkg = Package.get();
        pkg.read(res);
        String launcher = pkg.launcher();
        android.util.Log.i("snx", "package " + pkg.name() + ", application " + pkg.application.className + ", launcher " + launcher);

        ClassLoader loader = new AppClassLoader(Main.class.getClassLoader());
        android.app.AppComponentFactory factory = android.app.AppComponentFactory.DEFAULT;
        if (pkg.application.appComponentFactory != null)
            factory = (android.app.AppComponentFactory) Class.forName(pkg.application.appComponentFactory, true, loader).getDeclaredConstructor().newInstance();
        loader = factory.instantiateClassLoader(loader, pkg.application);
        App.set(loader, factory, res);

        String appClass = pkg.application.className != null ? pkg.application.className : "android.app.Application";
        android.app.Application application = factory.instantiateApplication(loader, appClass);
        ContextImpl base = new ContextImpl(res, false);
        base.setTheme(pkg.application.theme);
        application.attachContext(base);
        base.setOuter(application);
        App.setApplication(application);
        Providers.install();
        application.onCreate();

        if (launcher == null) throw new IllegalStateException("the manifest has no launcher activity");
        Activities.launchMain(launcher);
        android.os.Looper.loop();
    }
}
