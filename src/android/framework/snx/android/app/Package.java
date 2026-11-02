// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.res.Resources;
import android.content.res.XmlBlock;
import android.content.res.XmlResourceParser;
import android.os.Bundle;
import android.util.TypedValue;
import java.util.ArrayList;
import java.util.HashMap;
import org.xmlpull.v1.XmlPullParser;
import snx.android.Apk;

/**
 * The app's package, read from its manifest as Android's PackageParser reads it: the application, its activities,
 * services, receivers and providers with their meta-data and intent filters' actions.
 */
public final class Package {
    static final String ANDROID = "http://schemas.android.com/apk/res/android";

    public final PackageInfo info = new PackageInfo();
    public final ApplicationInfo application = new ApplicationInfo();
    public final ArrayList<ActivityInfo> activities = new ArrayList<>();
    public final ArrayList<ActivityInfo> receivers = new ArrayList<>();
    public final ArrayList<ServiceInfo> services = new ArrayList<>();
    public final ArrayList<ProviderInfo> providers = new ArrayList<>();
    /** The actions of each component's intent filters, and its categories, by its class name. */
    public final HashMap<String, ArrayList<String>> actions = new HashMap<>();
    public final HashMap<String, ArrayList<String>> categories = new HashMap<>();
    public final ArrayList<String> permissions = new ArrayList<>();
    public final HashMap<String, Integer> enabledStates = new HashMap<>();
    private static Package current;

    private Package() {}

    public static synchronized Package get() {
        if (current == null) current = new Package();
        return current;
    }

    public String name() { return info.packageName; }

    /** The manifest read, with resources to resolve its references. */
    public void read(Resources res) throws Exception {
        byte[] data = Apk.read("AndroidManifest.xml");
        if (data == null) throw new IllegalStateException("the APK has no AndroidManifest.xml");
        try (XmlBlock block = new XmlBlock(data); XmlResourceParser p = block.newParser()) {
            ComponentInfo component = null;
            ArrayList<String> filterActions = null, filterCategories = null;
            for (int e = p.next(); e != XmlPullParser.END_DOCUMENT; e = p.next()) {
                if (e == XmlPullParser.END_TAG) {
                    String tag = p.getName();
                    if (tag.equals("activity") || tag.equals("activity-alias") || tag.equals("service") || tag.equals("receiver") || tag.equals("provider"))
                        component = null;
                    continue;
                }
                if (e != XmlPullParser.START_TAG) continue;
                switch (p.getName()) {
                    case "manifest" -> {
                        info.packageName = p.getAttributeValue(null, "package");
                        info.versionCode = p.getAttributeIntValue(ANDROID, "versionCode", 0);
                        info.setLongVersionCode(info.versionCode);
                        info.versionName = p.getAttributeValue(ANDROID, "versionName");
                        application.packageName = info.packageName;
                        application.processName = info.packageName;
                        application.compileSdkVersion = p.getAttributeIntValue(ANDROID, "compileSdkVersion", 0);
                    }
                    case "uses-sdk" -> {
                        application.minSdkVersion = p.getAttributeIntValue(ANDROID, "minSdkVersion", 1);
                        application.targetSdkVersion = p.getAttributeIntValue(ANDROID, "targetSdkVersion", application.minSdkVersion);
                    }
                    case "uses-permission" -> permissions.add(p.getAttributeValue(ANDROID, "name"));
                    case "application" -> {
                        application.className = className(p.getAttributeValue(ANDROID, "name"));
                        application.name = application.className;
                        application.theme = p.getAttributeResourceValue(ANDROID, "theme", 0);
                        application.icon = p.getAttributeResourceValue(ANDROID, "icon", 0);
                        label(p, application, res);
                        application.appComponentFactory = className(p.getAttributeValue(ANDROID, "appComponentFactory"));
                        if (p.getAttributeBooleanValue(ANDROID, "debuggable", false)) application.flags |= ApplicationInfo.FLAG_DEBUGGABLE;
                        if (p.getAttributeBooleanValue(ANDROID, "allowBackup", true)) application.flags |= ApplicationInfo.FLAG_ALLOW_BACKUP;
                        if (p.getAttributeBooleanValue(ANDROID, "largeHeap", false)) application.flags |= ApplicationInfo.FLAG_LARGE_HEAP;
                        application.enabled = p.getAttributeBooleanValue(ANDROID, "enabled", true);
                    }
                    case "activity", "activity-alias" -> {
                        ActivityInfo a = new ActivityInfo();
                        component(p, a, res);
                        if (p.getName().equals("activity-alias")) a.targetActivity = className(p.getAttributeValue(ANDROID, "targetActivity"));
                        a.theme = p.getAttributeResourceValue(ANDROID, "theme", 0);
                        a.launchMode = p.getAttributeIntValue(ANDROID, "launchMode", 0);
                        a.screenOrientation = p.getAttributeIntValue(ANDROID, "screenOrientation", -1);
                        a.configChanges = p.getAttributeIntValue(ANDROID, "configChanges", 0);
                        a.softInputMode = p.getAttributeIntValue(ANDROID, "windowSoftInputMode", 0);
                        activities.add(a);
                        component = a;
                    }
                    case "receiver" -> {
                        ActivityInfo r = new ActivityInfo();
                        component(p, r, res);
                        receivers.add(r);
                        component = r;
                    }
                    case "service" -> {
                        ServiceInfo s = new ServiceInfo();
                        component(p, s, res);
                        s.permission = p.getAttributeValue(ANDROID, "permission");
                        services.add(s);
                        component = s;
                    }
                    case "provider" -> {
                        ProviderInfo pr = new ProviderInfo();
                        component(p, pr, res);
                        pr.authority = p.getAttributeValue(ANDROID, "authorities");
                        pr.readPermission = p.getAttributeValue(ANDROID, "readPermission");
                        pr.writePermission = p.getAttributeValue(ANDROID, "writePermission");
                        pr.grantUriPermissions = p.getAttributeBooleanValue(ANDROID, "grantUriPermissions", false);
                        pr.multiprocess = p.getAttributeBooleanValue(ANDROID, "multiprocess", false);
                        pr.initOrder = p.getAttributeIntValue(ANDROID, "initOrder", 0);
                        providers.add(pr);
                        component = pr;
                    }
                    case "intent-filter" -> {
                        filterActions = component != null ? actions.computeIfAbsent(component.name, k -> new ArrayList<>()) : null;
                        filterCategories = component != null ? categories.computeIfAbsent(component.name, k -> new ArrayList<>()) : null;
                    }
                    case "action" -> { if (filterActions != null) filterActions.add(p.getAttributeValue(ANDROID, "name")); }
                    case "category" -> { if (filterCategories != null) filterCategories.add(p.getAttributeValue(ANDROID, "name")); }
                    case "meta-data" -> metaData(p, component != null ? component : application, res);
                    default -> {}
                }
            }
        }
        // the paths of an installed package: the APK, and the data folders under the data root
        application.sourceDir = application.publicSourceDir = Apk.path();
        String root = snx.android.Main.dataRoot().getPath();
        application.dataDir = root;
        application.deviceProtectedDataDir = root + "/de";
        application.nativeLibraryDir = Apk.libraryDir();
        String[] splits = Apk.splitPaths();
        if (splits.length > 0) {
            application.splitSourceDirs = application.splitPublicSourceDirs = splits;
            application.splitNames = Apk.splitNames();
            info.splitNames = application.splitNames;
        }
        application.uid = android.os.Process.myUid();
        info.applicationInfo = application;
        info.activities = activities.toArray(new ActivityInfo[0]);
        info.receivers = receivers.toArray(new ActivityInfo[0]);
        info.services = services.toArray(new ServiceInfo[0]);
        info.providers = providers.toArray(new ProviderInfo[0]);
        info.requestedPermissions = permissions.toArray(new String[0]);
        info.firstInstallTime = info.lastUpdateTime = new java.io.File(Apk.path()).lastModified();
        info.signatures = new android.content.pm.Signature[] {new android.content.pm.Signature("5ca71e7a")};
        info.signingInfo = new android.content.pm.SigningInfo(info.signatures);
    }

    private String className(String name) { return snx.android.Main.className(info.packageName, name); }

    private void label(XmlResourceParser p, android.content.pm.PackageItemInfo item, Resources res) {
        int labelRes = p.getAttributeResourceValue(ANDROID, "label", 0);
        if (labelRes != 0) item.labelRes = labelRes;
        else item.nonLocalizedLabel = p.getAttributeValue(ANDROID, "label");
    }

    private void component(XmlResourceParser p, ComponentInfo c, Resources res) {
        c.packageName = info.packageName;
        c.name = className(p.getAttributeValue(ANDROID, "name"));
        c.applicationInfo = application;
        c.processName = application.processName;
        c.exported = p.getAttributeBooleanValue(ANDROID, "exported", false);
        c.enabled = p.getAttributeBooleanValue(ANDROID, "enabled", true);
        c.directBootAware = p.getAttributeBooleanValue(ANDROID, "directBootAware", false);
        c.icon = p.getAttributeResourceValue(ANDROID, "icon", 0);
        label(p, c, res);
    }

    /** A meta-data's value into the item's bundle: a resource ID, or the value's type. */
    private void metaData(XmlResourceParser p, android.content.pm.PackageItemInfo item, Resources res) {
        String name = p.getAttributeValue(ANDROID, "name");
        if (name == null) return;
        if (item.metaData == null) item.metaData = new Bundle();
        int resource = p.getAttributeResourceValue(ANDROID, "resource", 0);
        if (resource != 0) {
            item.metaData.putInt(name, resource);
            return;
        }
        int i = -1;
        for (int k = 0; k < p.getAttributeCount(); ++k) if (p.getAttributeNameResource(k) == 0x01010024) i = k; // android:value
        if (i < 0) return;
        XmlBlock.Parser bp = (XmlBlock.Parser) p;
        int type = bp.getAttributeDataType(i), data = bp.getAttributeData(i);
        if (type == TypedValue.TYPE_REFERENCE && data != 0) {
            TypedValue v = new TypedValue();
            try {
                res.getValue(data, v, true);
                type = v.type;
                data = v.data;
                if (type == TypedValue.TYPE_STRING) { item.metaData.putString(name, String.valueOf(v.string)); return; }
            } catch (Resources.NotFoundException e) {
                item.metaData.putInt(name, data);
                return;
            }
        }
        if (type == TypedValue.TYPE_INT_BOOLEAN) item.metaData.putBoolean(name, data != 0);
        else if (type == TypedValue.TYPE_FLOAT) item.metaData.putFloat(name, Float.intBitsToFloat(data));
        else if (type >= TypedValue.TYPE_FIRST_INT && type <= TypedValue.TYPE_LAST_INT) item.metaData.putInt(name, data);
        else item.metaData.putString(name, p.getAttributeValue(i));
    }

    public ActivityInfo activity(String className) {
        for (ActivityInfo a : activities) if (a.name.equals(className)) return a;
        return null;
    }
    public ServiceInfo service(String className) {
        for (ServiceInfo s : services) if (s.name.equals(className)) return s;
        return null;
    }
    public ActivityInfo receiver(String className) {
        for (ActivityInfo r : receivers) if (r.name.equals(className)) return r;
        return null;
    }
    public ProviderInfo provider(String authority) {
        for (ProviderInfo p : providers) {
            if (p.authority == null) continue;
            for (String a : p.authority.split(";")) if (a.equals(authority)) return p;
        }
        return null;
    }
    public ProviderInfo providerByClass(String className) {
        for (ProviderInfo p : providers) if (p.name.equals(className)) return p;
        return null;
    }
    public String launcher() {
        for (ActivityInfo a : activities) {
            ArrayList<String> act = actions.get(a.name), cat = categories.get(a.name);
            if (act != null && act.contains("android.intent.action.MAIN") && cat != null && cat.contains("android.intent.category.LAUNCHER"))
                return a.targetActivity != null ? a.targetActivity : a.name;
        }
        return null;
    }
    public synchronized int enabledState(ComponentName c) { return enabledStates.getOrDefault(c.getClassName(), 0); }
    public synchronized void setEnabledState(ComponentName c, int state) { enabledStates.put(c.getClassName(), state); }
}
