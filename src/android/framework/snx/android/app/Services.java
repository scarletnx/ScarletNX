// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.app.Service;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;

/** The app's running services, as Android's ActiveServices for one process: started and bound, on the main thread. */
public final class Services {
    private static final class Running {
        final Service service;
        final ServiceInfo info;
        boolean started;
        int startId;
        IBinder binder;
        boolean bound;
        final ArrayList<ServiceConnection> connections = new ArrayList<>();
        Running(Service service, ServiceInfo info) { this.service = service; this.info = info; }
    }

    private static final HashMap<String, Running> running = new HashMap<>();
    private static final IdentityHashMap<ServiceConnection, Running> connections = new IdentityHashMap<>();
    private static final Handler main = new Handler(Looper.getMainLooper());

    private Services() {}

    private static ServiceInfo resolve(Intent intent) {
        ComponentName c = intent.getComponent();
        if (c != null) return Package.get().service(c.getClassName());
        ResolveInfo r = PackageManagerImpl.get().resolveService(intent, 0);
        return r != null ? r.serviceInfo : null;
    }

    /** The service of a class, made and created when it does not run. */
    private static synchronized Running create(ServiceInfo info, Intent intent) {
        Running r = running.get(info.name);
        if (r != null) return r;
        Service s;
        try {
            s = App.factory().instantiateService(App.classLoader(), info.name, intent);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException("Unable to instantiate service " + info.name, e);
        }
        ContextImpl base = App.context(0);
        s.attachService(base, App.application());
        base.setOuter(s);
        r = new Running(s, info);
        running.put(info.name, r);
        s.onCreate();
        return r;
    }

    /** A running service of the class, made when it does not run (for JobScheduler). */
    public static Service get(ComponentName c) {
        ServiceInfo info = Package.get().service(c.getClassName());
        return info != null ? create(info, new Intent().setComponent(c)).service : null;
    }

    static ComponentName start(Intent intent) {
        ServiceInfo info = resolve(intent);
        if (info == null) return null;
        main.post(() -> {
            Running r = create(info, intent);
            r.started = true;
            r.service.onStartCommand(intent, 0, ++r.startId);
        });
        return new ComponentName(info.packageName, info.name);
    }

    static boolean stop(Intent intent) {
        ServiceInfo info = resolve(intent);
        Running r;
        synchronized (Services.class) { r = info != null ? running.get(info.name) : null; }
        if (r == null) return false;
        main.post(() -> stop(r));
        return true;
    }
    public static void stop(Service service) {
        main.post(() -> {
            Running r;
            synchronized (Services.class) {
                r = null;
                for (Running x : running.values()) if (x.service == service) r = x;
            }
            if (r != null) stop(r);
        });
    }
    private static void stop(Running r) {
        r.started = false;
        if (!r.connections.isEmpty()) return;
        synchronized (Services.class) { running.remove(r.info.name); }
        r.service.onDestroy();
    }

    static boolean bind(Intent intent, ServiceConnection conn, int flags) {
        ServiceInfo info = resolve(intent);
        if (info == null) return false;
        main.post(() -> {
            Running r = create(info, intent);
            if (!r.bound) {
                r.binder = r.service.onBind(intent);
                r.bound = true;
            }
            r.connections.add(conn);
            synchronized (Services.class) { connections.put(conn, r); }
            if (r.binder != null) conn.onServiceConnected(new ComponentName(info.packageName, info.name), r.binder);
            else conn.onNullBinding(new ComponentName(info.packageName, info.name));
        });
        return true;
    }
    static void unbind(ServiceConnection conn) {
        main.post(() -> {
            Running r;
            synchronized (Services.class) { r = connections.remove(conn); }
            if (r == null) return;
            r.connections.remove(conn);
            if (r.connections.isEmpty()) {
                r.service.onUnbind(new Intent().setComponent(new ComponentName(r.info.packageName, r.info.name)));
                r.bound = false;
                if (!r.started) stop(r);
            }
        });
    }
}
