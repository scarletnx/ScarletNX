// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** An activity, service, provider or receiver of a package. */
public class ComponentInfo extends PackageItemInfo {
    public ApplicationInfo applicationInfo;
    public String processName, splitName, attributionTags[];
    public int descriptionRes;
    public boolean enabled = true, exported, directBootAware;

    public ComponentInfo() {}
    public ComponentInfo(ComponentInfo o) {
        super(o);
        applicationInfo = o.applicationInfo; processName = o.processName; descriptionRes = o.descriptionRes;
        enabled = o.enabled; exported = o.exported; directBootAware = o.directBootAware;
    }
    public boolean isEnabled() { return enabled && (applicationInfo == null || applicationInfo.enabled); }
    public final int getIconResource() { return icon != 0 ? icon : applicationInfo != null ? applicationInfo.icon : 0; }
}
