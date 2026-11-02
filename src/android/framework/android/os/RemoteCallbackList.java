// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;

/** Callbacks of other processes. Here none die, so it is a list. */
public class RemoteCallbackList<E extends IInterface> {
    private final LinkedHashMap<IBinder, Object[]> callbacks = new LinkedHashMap<>();
    private ArrayList<Object[]> active;
    private boolean killed;

    public boolean register(E callback) { return register(callback, null); }

    public boolean register(E callback, Object cookie) {
        synchronized (callbacks) {
            if (killed) return false;
            callbacks.put(callback.asBinder(), new Object[] {callback, cookie});
            return true;
        }
    }

    public boolean unregister(E callback) {
        synchronized (callbacks) { return callbacks.remove(callback.asBinder()) != null; }
    }

    public void kill() { synchronized (callbacks) { callbacks.clear(); killed = true; } }
    public void onCallbackDied(E callback) {}
    public void onCallbackDied(E callback, Object cookie) { onCallbackDied(callback); }

    public int beginBroadcast() {
        synchronized (callbacks) {
            if (active != null) throw new IllegalStateException("beginBroadcast() called while already in a broadcast");
            active = new ArrayList<>(callbacks.values());
            return active.size();
        }
    }

    @SuppressWarnings("unchecked")
    public E getBroadcastItem(int index) { return (E) active.get(index)[0]; }
    public Object getBroadcastCookie(int index) { return active.get(index)[1]; }
    public void finishBroadcast() { synchronized (callbacks) { active = null; } }
    public int getRegisteredCallbackCount() { synchronized (callbacks) { return callbacks.size(); } }

    @SuppressWarnings("unchecked")
    public E getRegisteredCallbackItem(int index) {
        synchronized (callbacks) {
            int i = 0;
            for (Map.Entry<IBinder, Object[]> e : callbacks.entrySet()) if (i++ == index) return (E) e.getValue()[0];
            return null;
        }
    }
}
