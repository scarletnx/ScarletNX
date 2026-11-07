// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.ComponentCallbacks2;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.IBinder;

/** A background component: started or bound in the process. */
public abstract class Service extends android.content.ContextWrapper implements ComponentCallbacks2 {
    public static final int START_STICKY_COMPATIBILITY = 0, START_STICKY = 1, START_NOT_STICKY = 2, START_REDELIVER_INTENT = 3,
        START_CONTINUATION_MASK = 0xf, START_FLAG_REDELIVERY = 1, START_FLAG_RETRY = 2;
    public static final int STOP_FOREGROUND_REMOVE = 1, STOP_FOREGROUND_DETACH = 2, STOP_FOREGROUND_LEGACY = 0;
    private Application application;

    public Service() { super(null); }

    /** The runtime's attach: the context and the application. */
    public final void attachService(android.content.Context base, Application app) { attachBaseContext(base); application = app; }
    public final Application getApplication() { return application; }
    public void onCreate() {}
    @Deprecated public void onStart(Intent intent, int startId) {}
    public int onStartCommand(Intent intent, int flags, int startId) { onStart(intent, startId); return START_STICKY; }
    public void onDestroy() {}
    @Override public void onConfigurationChanged(Configuration newConfig) {}
    @Override public void onLowMemory() {}
    @Override public void onTrimMemory(int level) {}
    public abstract IBinder onBind(Intent intent);
    public boolean onUnbind(Intent intent) { return false; }
    public void onRebind(Intent intent) {}
    public void onTaskRemoved(Intent rootIntent) {}
    public void onTimeout(int startId) {}
    public void onTimeout(int startId, int fgsType) {}
    public final void stopSelf() { snx.android.app.Services.stop(this); }
    public final void stopSelf(int startId) { stopSelf(); }
    public final boolean stopSelfResult(int startId) { stopSelf(); return true; }
    public final void startForeground(int id, Notification notification) {}
    public final void startForeground(int id, Notification notification, int foregroundServiceType) {}
    @Deprecated public final void stopForeground(boolean removeNotification) {}
    public final void stopForeground(int notificationBehavior) {}
    public final int getForegroundServiceType() { return 0; }
}
