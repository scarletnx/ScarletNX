// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.Intent;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;

/** A service (deprecated) that handles its start intents one at a time on a worker thread, and stops when none remain. */
@Deprecated
public abstract class IntentService extends Service {
    private final String name;
    private boolean redelivery;
    private volatile int lastStartId;
    private HandlerThread thread;
    private Handler handler;

    public IntentService(String name) { this.name = name; }
    public void setIntentRedelivery(boolean enabled) { redelivery = enabled; }
    @Override public void onCreate() {
        super.onCreate();
        thread = new HandlerThread("IntentService[" + name + "]");
        thread.start();
        handler = new Handler(thread.getLooper(), msg -> {
            onHandleIntent((Intent) msg.obj);
            if (msg.arg1 == lastStartId) stopSelf(msg.arg1); // the latest start only, as stopSelf(int) does on Android
            return true;
        });
    }
    @Override public void onStart(Intent intent, int startId) {
        android.os.Message m = handler.obtainMessage(0, intent);
        m.arg1 = lastStartId = startId;
        handler.sendMessage(m);
    }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        onStart(intent, startId);
        return redelivery ? START_REDELIVER_INTENT : START_NOT_STICKY;
    }
    @Override public void onDestroy() { thread.quit(); }
    @Override public IBinder onBind(Intent intent) { return null; }
    protected abstract void onHandleIntent(Intent intent);
}
