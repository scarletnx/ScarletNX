// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
//
// The framework's members as an app's code links to them, as Android 16's SDK declares them: a call finds its method
// by name and descriptor. Run as a source file, with the framework jar in a loader of its own:
//   java tests/android_framework_api.java scarletnx-android-framework.jar
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;

final class AndroidFrameworkApi {
    private static int failures;
    private static ClassLoader framework;

    private static void check(boolean ok, String what, String got) {
        if (ok) return;
        System.err.println("android_framework_api: " + what + " (got: " + got + ")");
        ++failures;
    }

    private static Class<?> type(String name) throws ClassNotFoundException {
        return Class.forName(name, false, framework);
    }

    /** The methods of that name, as the framework declares them. */
    private static String named(Class<?> c, String name) {
        StringBuilder got = new StringBuilder();
        for (Method m : c.getMethods()) {
            if (m.getName().equals(name)) got.append(got.length() > 0 ? ", " : "").append(m);
        }
        return got.length() > 0 ? got.toString() : "no method " + name;
    }

    /** An app's call of cls.name, with the SDK's return and parameter types, finds the method. */
    private static void links(String cls, String name, Class<?> ret, Class<?>... params)
            throws ReflectiveOperationException {
        Class<?> c = type(cls);
        MethodType t = MethodType.methodType(ret, params);
        try {
            MethodHandles.publicLookup().findVirtual(c, name, t);
        } catch (NoSuchMethodException e) {
            check(false, cls + "." + name + t.toMethodDescriptorString()
                    + " is what an app's code calls: it finds no method", named(c, name));
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            System.err.println("usage: java android_framework_api.java FRAMEWORK_JAR");
            System.exit(2);
        }
        URL jar = Path.of(args[0]).toUri().toURL();
        try (URLClassLoader loader = new URLClassLoader(new URL[] {jar}, ClassLoader.getPlatformClassLoader())) {
            framework = loader;
            links("android.net.LinkProperties", "getDhcpServerAddress", java.net.Inet4Address.class);
        }
        if (failures != 0) System.exit(1);
        System.out.println("android_framework_api: ok");
    }
}
