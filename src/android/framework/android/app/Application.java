// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.ComponentCallbacks;
import android.content.ComponentCallbacks2;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.res.Configuration;
import android.os.Bundle;
import java.util.ArrayList;

/** The app's process-wide state: its first component, with the callbacks of every activity. */
public class Application extends ContextWrapper implements ComponentCallbacks2 {
    private final ArrayList<ActivityLifecycleCallbacks> activityCallbacks = new ArrayList<>();
    private final ArrayList<ComponentCallbacks> componentCallbacks = new ArrayList<>();
    private final ArrayList<OnProvideAssistDataListener> assistListeners = new ArrayList<>();

    public interface ActivityLifecycleCallbacks {
        default void onActivityPreCreated(Activity activity, Bundle savedInstanceState) {}
        void onActivityCreated(Activity activity, Bundle savedInstanceState);
        default void onActivityPostCreated(Activity activity, Bundle savedInstanceState) {}
        default void onActivityPreStarted(Activity activity) {}
        void onActivityStarted(Activity activity);
        default void onActivityPostStarted(Activity activity) {}
        default void onActivityPreResumed(Activity activity) {}
        void onActivityResumed(Activity activity);
        default void onActivityPostResumed(Activity activity) {}
        default void onActivityPrePaused(Activity activity) {}
        void onActivityPaused(Activity activity);
        default void onActivityPostPaused(Activity activity) {}
        default void onActivityPreStopped(Activity activity) {}
        void onActivityStopped(Activity activity);
        default void onActivityPostStopped(Activity activity) {}
        default void onActivityPreSaveInstanceState(Activity activity, Bundle outState) {}
        void onActivitySaveInstanceState(Activity activity, Bundle outState);
        default void onActivityPostSaveInstanceState(Activity activity, Bundle outState) {}
        default void onActivityPreDestroyed(Activity activity) {}
        void onActivityDestroyed(Activity activity);
        default void onActivityPostDestroyed(Activity activity) {}
    }

    public interface OnProvideAssistDataListener {
        void onProvideAssistData(Activity activity, Bundle data);
    }

    public Application() { super(null); }

    /** The runtime gives the application its context before onCreate, as Android's attach does. */
    public final void attachContext(Context base) { attachBaseContext(base); }

    public void onCreate() {}
    public void onTerminate() {}
    @Override public void onConfigurationChanged(Configuration newConfig) {
        for (ComponentCallbacks c : componentCallbacks()) c.onConfigurationChanged(newConfig);
    }
    @Override public void onLowMemory() { for (ComponentCallbacks c : componentCallbacks()) c.onLowMemory(); }
    @Override public void onTrimMemory(int level) {
        for (ComponentCallbacks c : componentCallbacks()) if (c instanceof ComponentCallbacks2 c2) c2.onTrimMemory(level);
    }
    private ComponentCallbacks[] componentCallbacks() {
        synchronized (componentCallbacks) { return componentCallbacks.toArray(new ComponentCallbacks[0]); }
    }
    @Override public void registerComponentCallbacks(ComponentCallbacks callback) { synchronized (componentCallbacks) { componentCallbacks.add(callback); } }
    @Override public void unregisterComponentCallbacks(ComponentCallbacks callback) { synchronized (componentCallbacks) { componentCallbacks.remove(callback); } }
    public void registerActivityLifecycleCallbacks(ActivityLifecycleCallbacks callback) { synchronized (activityCallbacks) { activityCallbacks.add(callback); } }
    public void unregisterActivityLifecycleCallbacks(ActivityLifecycleCallbacks callback) { synchronized (activityCallbacks) { activityCallbacks.remove(callback); } }
    public void registerOnProvideAssistDataListener(OnProvideAssistDataListener callback) { synchronized (assistListeners) { assistListeners.add(callback); } }
    public void unregisterOnProvideAssistDataListener(OnProvideAssistDataListener callback) { synchronized (assistListeners) { assistListeners.remove(callback); } }
    public static String getProcessName() { return snx.android.Main.processName(); }

    ActivityLifecycleCallbacks[] activityCallbacks() {
        synchronized (activityCallbacks) { return activityCallbacks.toArray(new ActivityLifecycleCallbacks[0]); }
    }
}
