// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.appwidget;

import android.os.Bundle;

import android.content.ComponentName;
import android.content.Context;
import android.widget.RemoteViews;

/** The home screen's widgets. There is no home screen, so no widget is placed and an update goes nowhere. */
public class AppWidgetManager {
    public static final String ACTION_APPWIDGET_UPDATE = "android.appwidget.action.APPWIDGET_UPDATE",
        ACTION_APPWIDGET_DELETED = "android.appwidget.action.APPWIDGET_DELETED",
        ACTION_APPWIDGET_DISABLED = "android.appwidget.action.APPWIDGET_DISABLED",
        ACTION_APPWIDGET_ENABLED = "android.appwidget.action.APPWIDGET_ENABLED",
        ACTION_APPWIDGET_OPTIONS_CHANGED = "android.appwidget.action.APPWIDGET_UPDATE_OPTIONS",
        ACTION_APPWIDGET_RESTORED = "android.appwidget.action.APPWIDGET_RESTORED";
    public static final String EXTRA_APPWIDGET_OPTIONS = "appWidgetOptions", EXTRA_APPWIDGET_OLD_IDS = "appWidgetOldIds",
        EXTRA_APPWIDGET_PROVIDER = "appWidgetProvider";
    public static final String EXTRA_APPWIDGET_ID = "appWidgetId", EXTRA_APPWIDGET_IDS = "appWidgetIds";
    public static final int INVALID_APPWIDGET_ID = 0;
    private static final AppWidgetManager INSTANCE = new AppWidgetManager();

    private AppWidgetManager() {}
    public static AppWidgetManager getInstance(Context context) { return INSTANCE; }
    public int[] getAppWidgetIds(ComponentName provider) { return new int[0]; }
    /* no launcher shows widgets here: none to pin, and none with options */
    public Bundle getAppWidgetOptions(int appWidgetId) { return new Bundle(); }
    public void updateAppWidgetOptions(int appWidgetId, Bundle options) {}
    public boolean isRequestPinAppWidgetSupported() { return false; }
    public boolean requestPinAppWidget(ComponentName provider, Bundle extras, android.app.PendingIntent successCallback) { return false; }
    public void notifyAppWidgetViewDataChanged(int appWidgetId, int viewId) {}
    public void notifyAppWidgetViewDataChanged(int[] appWidgetIds, int viewId) {}
    public void updateAppWidget(int[] appWidgetIds, RemoteViews views) {}
    public void updateAppWidget(int appWidgetId, RemoteViews views) {}
    public void updateAppWidget(ComponentName provider, RemoteViews views) {}
}
