// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Intent;
import android.os.IBinder;

/** The service that gives a collection widget its views: no host binds to it here, as no launcher shows widgets */
public abstract class RemoteViewsService extends android.app.Service {
    public interface RemoteViewsFactory {
        void onCreate();
        void onDataSetChanged();
        void onDestroy();
        int getCount();
        RemoteViews getViewAt(int position);
        RemoteViews getLoadingView();
        int getViewTypeCount();
        long getItemId(int position);
        boolean hasStableIds();
    }
    public RemoteViewsService() {}
    public abstract RemoteViewsFactory onGetViewFactory(Intent intent);
    @Override public IBinder onBind(Intent intent) { return null; }
}
