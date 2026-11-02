// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.Resources;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** The package manager of a device where this app is the only package: its manifest answers each query. */
public final class PackageManagerImpl extends PackageManager {
    private static final PackageManagerImpl instance = new PackageManagerImpl();

    private PackageManagerImpl() {}

    public static PackageManagerImpl get() { return instance; }

    private static Package pkg() { return Package.get(); }
    private static void check(String packageName) throws NameNotFoundException {
        if (!pkg().name().equals(packageName)) throw new NameNotFoundException(packageName);
    }
    private static <T extends ComponentInfo> T check(T info, ComponentName c) throws NameNotFoundException {
        if (info == null) throw new NameNotFoundException(c.flattenToString());
        return info;
    }
    private boolean enabled(ComponentInfo c) {
        int state = pkg().enabledState(new ComponentName(c.packageName, c.name));
        return state == COMPONENT_ENABLED_STATE_DEFAULT ? c.enabled : state == COMPONENT_ENABLED_STATE_ENABLED;
    }

    @Override public PackageInfo getPackageInfo(String packageName, int flags) throws NameNotFoundException { check(packageName); return pkg().info; }
    @Override public ApplicationInfo getApplicationInfo(String packageName, int flags) throws NameNotFoundException { check(packageName); return pkg().application; }
    @Override public ActivityInfo getActivityInfo(ComponentName c, int flags) throws NameNotFoundException { return check(pkg().activity(c.getClassName()), c); }
    @Override public ActivityInfo getReceiverInfo(ComponentName c, int flags) throws NameNotFoundException { return check(pkg().receiver(c.getClassName()), c); }
    @Override public ServiceInfo getServiceInfo(ComponentName c, int flags) throws NameNotFoundException { return check(pkg().service(c.getClassName()), c); }
    @Override public ProviderInfo getProviderInfo(ComponentName c, int flags) throws NameNotFoundException { return check(pkg().providerByClass(c.getClassName()), c); }
    @Override public ProviderInfo resolveContentProvider(String authority, int flags) { return pkg().provider(authority); }
    @Override public List<PackageInfo> getInstalledPackages(int flags) { return Collections.singletonList(pkg().info); }
    @Override public List<ApplicationInfo> getInstalledApplications(int flags) { return Collections.singletonList(pkg().application); }

    /** Whether a component's intent filters take the intent: its explicit component, or its action and categories. */
    private boolean matches(ComponentInfo c, Intent intent) {
        ComponentName explicit = intent.getComponent();
        if (explicit != null) return explicit.getClassName().equals(c.name);
        if (intent.getPackage() != null && !intent.getPackage().equals(pkg().name())) return false;
        List<String> actions = pkg().actions.get(c.name), categories = pkg().categories.get(c.name);
        if (actions == null || intent.getAction() == null || !actions.contains(intent.getAction())) return false;
        if (intent.getCategories() != null)
            for (String category : intent.getCategories()) if (categories == null || !categories.contains(category)) return false;
        return true;
    }
    private <T extends ComponentInfo> List<ResolveInfo> query(List<T> components, Intent intent, int flags, java.util.function.BiConsumer<ResolveInfo, T> set) {
        ArrayList<ResolveInfo> out = new ArrayList<>();
        for (T c : components) {
            if (!matches(c, intent) || ((flags & MATCH_DISABLED_COMPONENTS) == 0 && !enabled(c))) continue;
            ResolveInfo r = new ResolveInfo();
            set.accept(r, c);
            r.resolvePackageName = c.packageName;
            r.isDefault = true;
            out.add(r);
        }
        return out;
    }
    @Override public List<ResolveInfo> queryIntentActivities(Intent intent, int flags) { return query(pkg().activities, intent, flags, (r, c) -> r.activityInfo = c); }
    @Override public List<ResolveInfo> queryIntentServices(Intent intent, int flags) { return query(pkg().services, intent, flags, (r, c) -> r.serviceInfo = c); }
    @Override public List<ResolveInfo> queryBroadcastReceivers(Intent intent, int flags) { return query(pkg().receivers, intent, flags, (r, c) -> r.activityInfo = c); }
    @Override public List<ResolveInfo> queryIntentContentProviders(Intent intent, int flags) { return query(pkg().providers, intent, flags, (r, c) -> r.providerInfo = c); }
    @Override public ResolveInfo resolveActivity(Intent intent, int flags) { List<ResolveInfo> l = queryIntentActivities(intent, flags); return l.isEmpty() ? null : l.get(0); }
    @Override public ResolveInfo resolveService(Intent intent, int flags) { List<ResolveInfo> l = queryIntentServices(intent, flags); return l.isEmpty() ? null : l.get(0); }
    @Override public Intent getLaunchIntentForPackage(String packageName) {
        String launcher = pkg().launcher();
        if (launcher == null || !packageName.equals(pkg().name())) return null;
        return new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setClassName(packageName, launcher)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    }
    @Override public int checkPermission(String permName, String packageName) {
        return pkg().name().equals(packageName) && pkg().permissions.contains(permName) ? PERMISSION_GRANTED : PERMISSION_DENIED;
    }
    /** The features of a phone with a touchscreen: no camera, telephony or sensors. */
    @Override public boolean hasSystemFeature(String featureName) {
        return switch (featureName) {
            case "android.hardware.touchscreen", "android.hardware.touchscreen.multitouch", "android.hardware.screen.portrait",
                 "android.hardware.screen.landscape", "android.software.app_widgets", "android.hardware.faketouch",
                 "android.hardware.vulkan.level", "android.hardware.vulkan.version", "android.hardware.wifi" -> true;
            default -> false;
        };
    }
    @Override public synchronized void setComponentEnabledSetting(ComponentName componentName, int newState, int flags) { pkg().setEnabledState(componentName, newState); }
    @Override public synchronized int getComponentEnabledSetting(ComponentName componentName) { return pkg().enabledState(componentName); }
    @Override public void setApplicationEnabledSetting(String packageName, int newState, int flags) {}
    @Override public int getApplicationEnabledSetting(String packageName) { return COMPONENT_ENABLED_STATE_DEFAULT; }
    @Override public String getInstallerPackageName(String packageName) { return null; }
    @Override public CharSequence getApplicationLabel(ApplicationInfo info) { return info.loadLabel(this); }
    @Override public Drawable getApplicationIcon(ApplicationInfo info) { return new ColorDrawable(0); }
    @Override public Drawable getApplicationIcon(String packageName) throws NameNotFoundException { check(packageName); return new ColorDrawable(0); }
    @Override public CharSequence getText(String packageName, int resid, ApplicationInfo appInfo) {
        try {
            return App.resources().getText(resid);
        } catch (Resources.NotFoundException e) {
            return null;
        }
    }
    @Override public Resources getResourcesForApplication(ApplicationInfo app) throws NameNotFoundException { check(app.packageName); return App.resources(); }
    @Override public Resources getResourcesForApplication(String packageName) throws NameNotFoundException { check(packageName); return App.resources(); }
    @Override public String[] getPackagesForUid(int uid) { return uid == android.os.Process.myUid() ? new String[] {pkg().name()} : null; }
    @Override public String getNameForUid(int uid) { return uid == android.os.Process.myUid() ? pkg().name() : null; }
    @Override public int getPackageUid(String packageName, int flags) throws NameNotFoundException { check(packageName); return android.os.Process.myUid(); }
    @Override public boolean isSafeMode() { return false; }
    @Override public PermissionInfo getPermissionInfo(String permName, int flags) throws NameNotFoundException {
        PermissionInfo p = new PermissionInfo();
        p.name = permName;
        p.packageName = "android";
        return p;
    }
    @Override public int checkSignatures(String packageName1, String packageName2) {
        return packageName1.equals(packageName2) && packageName1.equals(pkg().name()) ? SIGNATURE_MATCH : SIGNATURE_UNKNOWN_PACKAGE;
    }
}
