// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.appwidget;

import android.content.ComponentName;
import android.content.Context;
import android.widget.RemoteViews;

/** The home screen's widgets. There is no home screen, so no widget is placed and an update goes nowhere. */
public class AppWidgetManager {
    public static final String ACTION_APPWIDGET_UPDATE = "android.appwidget.action.APPWIDGET_UPDATE";
    public static final String EXTRA_APPWIDGET_ID = "appWidgetId", EXTRA_APPWIDGET_IDS = "appWidgetIds";
    public static final int INVALID_APPWIDGET_ID = 0;
    private static final AppWidgetManager INSTANCE = new AppWidgetManager();

    private AppWidgetManager() {}
    public static AppWidgetManager getInstance(Context context) { return INSTANCE; }
    public int[] getAppWidgetIds(ComponentName provider) { return new int[0]; }
    public void updateAppWidget(int[] appWidgetIds, RemoteViews views) {}
    public void updateAppWidget(int appWidgetId, RemoteViews views) {}
    public void updateAppWidget(ComponentName provider, RemoteViews views) {}
}
