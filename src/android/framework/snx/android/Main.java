// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android;

import android.content.res.XmlBlock;
import android.content.res.XmlResourceParser;
import org.xmlpull.v1.XmlPullParser;
import snx.dalvik.AppClassLoader;

/** Where the runtime starts the app: the manifest read, the app's classes loaded and its components started. */
public final class Main {
    static final String ANDROID = "http://schemas.android.com/apk/res/android";

    private Main() {}

    /** The app's package name, its Application class (null for android.app.Application), its launcher activity. */
    record Manifest(String pkg, String application, String launcher) {}

    /** A component's class name: one the manifest gives as ".Name" or "Name" is in the app's package. */
    static String className(String pkg, String name) {
        if (name == null) return null;
        if (name.startsWith(".")) return pkg + name;
        return name.indexOf('.') < 0 ? pkg + "." + name : name;
    }

    static Manifest readManifest() throws Exception {
        byte[] data = Apk.read("AndroidManifest.xml");
        if (data == null) throw new IllegalStateException("the APK has no AndroidManifest.xml");
        String pkg = null, application = null, launcher = null;
        try (XmlBlock block = new XmlBlock(data); XmlResourceParser p = block.newParser()) {
            String activity = null;
            boolean main = false, launch = false;
            for (int e = p.next(); e != XmlPullParser.END_DOCUMENT; e = p.next()) {
                if (e == XmlPullParser.END_TAG) {
                    String tag = p.getName();
                    if ((tag.equals("activity") || tag.equals("activity-alias")) && launcher == null && main && launch)
                        launcher = activity;
                    continue;
                }
                if (e != XmlPullParser.START_TAG) continue;
                switch (p.getName()) {
                    case "manifest" -> pkg = p.getAttributeValue(null, "package");
                    case "application" -> application = className(pkg, p.getAttributeValue(ANDROID, "name"));
                    case "activity" -> {
                        activity = className(pkg, p.getAttributeValue(ANDROID, "name"));
                        main = launch = false;
                    }
                    case "activity-alias" -> {
                        activity = className(pkg, p.getAttributeValue(ANDROID, "targetActivity"));
                        main = launch = false;
                    }
                    case "action" -> main |= "android.intent.action.MAIN".equals(p.getAttributeValue(ANDROID, "name"));
                    case "category" ->
                        launch |= "android.intent.category.LAUNCHER".equals(p.getAttributeValue(ANDROID, "name"));
                    default -> {}
                }
            }
        }
        return new Manifest(pkg, application, launcher);
    }

    public static void main(String[] args) throws Throwable {
        Manifest manifest = readManifest();
        System.out.println("package " + manifest.pkg() + ", application " + manifest.application() + ", launcher "
            + manifest.launcher());
        ClassLoader app = new AppClassLoader(Main.class.getClassLoader());
        Class<?> activity = Class.forName(manifest.launcher(), false, app);
        for (Class<?> c = activity; c != null; c = c.getSuperclass()) System.out.println("  " + c.getName());
        activity.getDeclaredMethods(); // linked, so verified
    }
}
