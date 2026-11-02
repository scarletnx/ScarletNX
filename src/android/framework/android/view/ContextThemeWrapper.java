// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;

/** A context with a theme of its own. */
public class ContextThemeWrapper extends ContextWrapper {
    private int themeResource;
    private Resources.Theme theme;
    private LayoutInflater inflater;
    private Configuration overrideConfiguration;
    private Resources resources;

    public ContextThemeWrapper() { super(null); }
    public ContextThemeWrapper(Context base, int themeResId) { super(base); themeResource = themeResId; }
    public ContextThemeWrapper(Context base, Resources.Theme theme) { super(base); this.theme = theme; }

    @Override protected void attachBaseContext(Context newBase) { super.attachBaseContext(newBase); }

    public void applyOverrideConfiguration(Configuration overrideConfiguration) {
        if (resources != null) throw new IllegalStateException("getResources() or getAssets() has already been called");
        this.overrideConfiguration = new Configuration(overrideConfiguration);
    }

    public Configuration getOverrideConfiguration() { return overrideConfiguration; }

    @Override public AssetManager getAssets() { return getResources().getAssets(); }

    @Override public Resources getResources() {
        if (resources == null) {
            if (overrideConfiguration == null) resources = super.getResources();
            else {
                Configuration c = new Configuration(super.getResources().getConfiguration());
                c.updateFrom(overrideConfiguration);
                resources = new Resources(super.getResources().getAssets(), super.getResources().getDisplayMetrics(), c);
            }
        }
        return resources;
    }

    @Override public void setTheme(int resid) {
        if (themeResource != resid) {
            themeResource = resid;
            initializeTheme();
        }
    }

    public void setTheme(Resources.Theme theme) { this.theme = theme; }

    public int getThemeResId() { return themeResource; }

    @Override public Resources.Theme getTheme() {
        if (theme != null) return theme;
        if (themeResource == 0) themeResource = getBaseContext() != null ? getBaseContext().getApplicationInfo().theme : 0;
        initializeTheme();
        return theme;
    }

    private void initializeTheme() {
        boolean first = theme == null;
        if (first) {
            theme = getResources().newTheme();
            Resources.Theme base = getBaseContext() != null ? getBaseContext().getTheme() : null;
            if (base != null) theme.setTo(base);
        }
        onApplyThemeResource(theme, themeResource, first);
    }

    protected void onApplyThemeResource(Resources.Theme theme, int resid, boolean first) {
        if (resid != 0) theme.applyStyle(resid, true);
    }

    @Override public Object getSystemService(String name) {
        if (LAYOUT_INFLATER_SERVICE.equals(name)) {
            if (inflater == null) inflater = LayoutInflater.from(getBaseContext()).cloneInContext(this);
            return inflater;
        }
        return getBaseContext().getSystemService(name);
    }
}
