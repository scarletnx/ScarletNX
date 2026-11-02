// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

import android.os.Bundle;

/** An item of a package: its names, label, icon and meta-data. */
public class PackageItemInfo {
    public String name, packageName;
    public int labelRes, icon, banner, logo;
    public CharSequence nonLocalizedLabel;
    public Bundle metaData;

    public PackageItemInfo() {}
    public PackageItemInfo(PackageItemInfo o) {
        name = o.name; packageName = o.packageName; labelRes = o.labelRes; icon = o.icon; banner = o.banner; logo = o.logo;
        nonLocalizedLabel = o.nonLocalizedLabel; metaData = o.metaData;
    }
    public CharSequence loadLabel(PackageManager pm) {
        if (nonLocalizedLabel != null) return nonLocalizedLabel;
        CharSequence l = labelRes != 0 ? pm.getText(packageName, labelRes, null) : null;
        return l != null ? l : name != null ? name : packageName;
    }
    public android.graphics.drawable.Drawable loadIcon(PackageManager pm) { return null; }
    public android.content.res.XmlResourceParser loadXmlMetaData(PackageManager pm, String name) { return null; }
}
