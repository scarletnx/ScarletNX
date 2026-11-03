// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** The component an intent resolves to. */
public class ResolveInfo implements android.os.Parcelable {
    public ActivityInfo activityInfo;
    public ServiceInfo serviceInfo;
    public ProviderInfo providerInfo;
    public android.content.IntentFilter filter;
    public int priority, preferredOrder, match, specificIndex = -1, labelRes, icon;
    public CharSequence nonLocalizedLabel;
    public String resolvePackageName;
    public boolean isDefault;
    public CharSequence loadLabel(PackageManager pm) {
        ComponentInfo ci = activityInfo != null ? activityInfo : serviceInfo != null ? serviceInfo : providerInfo;
        return ci != null ? ci.loadLabel(pm) : nonLocalizedLabel;
    }
    public android.graphics.drawable.Drawable loadIcon(PackageManager pm) {
        ComponentInfo ci = activityInfo != null ? activityInfo : serviceInfo != null ? serviceInfo : providerInfo;
        if (icon != 0 && ci != null) {
            android.graphics.drawable.Drawable d = pm.getDrawable(ci.packageName, icon, ci.applicationInfo);
            if (d != null) return d;
        }
        return ci != null ? ci.loadIcon(pm) : null;
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int f) { dest.writeValue(this); }
}
