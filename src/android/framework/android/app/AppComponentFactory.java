// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.BroadcastReceiver;
import android.content.ContentProvider;
import android.content.Intent;
import android.content.pm.ApplicationInfo;

/** Makes the app's components: AndroidX installs its own, and the runtime calls it as Android's does. */
public class AppComponentFactory {
    public static final AppComponentFactory DEFAULT = new AppComponentFactory();

    public AppComponentFactory() {}

    private static Object make(ClassLoader cl, String className) throws ClassNotFoundException, InstantiationException, IllegalAccessException {
        try {
            return Class.forName(className, false, cl).getDeclaredConstructor().newInstance();
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw new RuntimeException(e.getCause());
        } catch (NoSuchMethodException e) {
            throw new InstantiationException(className + " has no constructor without arguments");
        }
    }

    public ClassLoader instantiateClassLoader(ClassLoader cl, ApplicationInfo aInfo) { return cl; }
    public Application instantiateApplication(ClassLoader cl, String className) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (Application) make(cl, className);
    }
    public Activity instantiateActivity(ClassLoader cl, String className, Intent intent) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (Activity) make(cl, className);
    }
    public BroadcastReceiver instantiateReceiver(ClassLoader cl, String className, Intent intent) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (BroadcastReceiver) make(cl, className);
    }
    public Service instantiateService(ClassLoader cl, String className, Intent intent) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (Service) make(cl, className);
    }
    public ContentProvider instantiateProvider(ClassLoader cl, String className) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return (ContentProvider) make(cl, className);
    }
}
