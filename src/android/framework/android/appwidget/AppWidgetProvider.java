// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.appwidget;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

/** A receiver of a widget's broadcasts, which it hands to its callbacks as Android's does */
public class AppWidgetProvider extends BroadcastReceiver {
    public AppWidgetProvider() {}

    @Override public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        Bundle extras = intent.getExtras();
        AppWidgetManager manager = AppWidgetManager.getInstance(context);
        if (AppWidgetManager.ACTION_APPWIDGET_UPDATE.equals(action)) {
            int[] ids = extras != null ? extras.getIntArray(AppWidgetManager.EXTRA_APPWIDGET_IDS) : null;
            if (ids != null && ids.length > 0) onUpdate(context, manager, ids);
        } else if (AppWidgetManager.ACTION_APPWIDGET_DELETED.equals(action)) {
            if (extras != null && extras.containsKey(AppWidgetManager.EXTRA_APPWIDGET_ID))
                onDeleted(context, new int[] {extras.getInt(AppWidgetManager.EXTRA_APPWIDGET_ID)});
        } else if (AppWidgetManager.ACTION_APPWIDGET_OPTIONS_CHANGED.equals(action)) {
            if (extras != null && extras.containsKey(AppWidgetManager.EXTRA_APPWIDGET_ID) && extras.containsKey(AppWidgetManager.EXTRA_APPWIDGET_OPTIONS))
                onAppWidgetOptionsChanged(context, manager, extras.getInt(AppWidgetManager.EXTRA_APPWIDGET_ID),
                    extras.getBundle(AppWidgetManager.EXTRA_APPWIDGET_OPTIONS));
        } else if (AppWidgetManager.ACTION_APPWIDGET_ENABLED.equals(action)) {
            onEnabled(context);
        } else if (AppWidgetManager.ACTION_APPWIDGET_DISABLED.equals(action)) {
            onDisabled(context);
        } else if (AppWidgetManager.ACTION_APPWIDGET_RESTORED.equals(action) && extras != null) {
            int[] oldIds = extras.getIntArray(AppWidgetManager.EXTRA_APPWIDGET_OLD_IDS), newIds = extras.getIntArray(AppWidgetManager.EXTRA_APPWIDGET_IDS);
            if (oldIds != null && oldIds.length > 0) {
                onRestored(context, oldIds, newIds);
                onUpdate(context, manager, newIds);
            }
        }
    }
    public void onUpdate(Context context, AppWidgetManager appWidgetManager, int[] appWidgetIds) {}
    public void onAppWidgetOptionsChanged(Context context, AppWidgetManager appWidgetManager, int appWidgetId, Bundle newOptions) {}
    public void onDeleted(Context context, int[] appWidgetIds) {}
    public void onEnabled(Context context) {}
    public void onDisabled(Context context) {}
    public void onRestored(Context context, int[] oldWidgetIds, int[] newWidgetIds) {}
}
