// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ResolveInfo;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;

/**
 * Broadcasts in the process: to the receivers that the app registers, and to the manifest's receivers that the intent
 * names or matches. No other app or the system sends any, so sticky broadcasts are none.
 */
final class Broadcasts {
    private record Registered(Context context, BroadcastReceiver receiver, IntentFilter filter, Handler handler) {}

    private static final ArrayList<Registered> registered = new ArrayList<>();

    private Broadcasts() {}

    static Intent register(Context context, BroadcastReceiver receiver, IntentFilter filter, Handler scheduler) {
        if (receiver == null) return sticky(filter);
        Handler h = scheduler != null ? scheduler : new Handler(Looper.getMainLooper());
        synchronized (registered) { registered.add(new Registered(context, receiver, filter, h)); }
        return sticky(filter);
    }

    /** The sticky broadcast a filter matches, as the system keeps it: the battery's state, full and on power */
    static Intent sticky(IntentFilter filter) {
        if (!filter.hasAction(Intent.ACTION_BATTERY_CHANGED)) return null;
        return new Intent(Intent.ACTION_BATTERY_CHANGED).putExtra("level", 100).putExtra("scale", 100)
            .putExtra("status", 5 /* BATTERY_STATUS_FULL */).putExtra("plugged", 1 /* BATTERY_PLUGGED_AC */)
            .putExtra("present", true).putExtra("health", 2 /* BATTERY_HEALTH_GOOD */);
    }

    static void unregister(BroadcastReceiver receiver) {
        synchronized (registered) {
            if (!registered.removeIf(r -> r.receiver() == receiver)) throw new IllegalArgumentException("Receiver not registered: " + receiver);
        }
    }

    private static boolean matches(IntentFilter f, Intent intent) {
        if (!f.matchAction(intent.getAction())) return false;
        if (intent.getCategories() != null) for (String c : intent.getCategories()) if (!f.hasCategory(c)) return false;
        return true;
    }

    static void send(Intent intent) {
        ArrayList<Registered> targets = new ArrayList<>();
        synchronized (registered) { for (Registered r : registered) if (intent.getComponent() == null && matches(r.filter(), intent)) targets.add(r); }
        for (Registered r : targets) r.handler().post(() -> r.receiver().onReceive(r.context(), intent));
        Handler main = new Handler(Looper.getMainLooper());
        for (ResolveInfo ri : PackageManagerImpl.get().queryBroadcastReceivers(intent, 0)) {
            ActivityInfo info = ri.activityInfo;
            main.post(() -> {
                BroadcastReceiver receiver;
                try {
                    receiver = App.factory().instantiateReceiver(App.classLoader(), info.name, intent);
                } catch (ReflectiveOperationException e) {
                    throw new RuntimeException("Unable to instantiate receiver " + info.name, e);
                }
                receiver.setPendingResult(new BroadcastReceiver.PendingResult());
                receiver.onReceive(App.application(), intent);
            });
        }
    }
}
