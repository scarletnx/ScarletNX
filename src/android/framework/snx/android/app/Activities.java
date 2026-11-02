// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ResolveInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;

/**
 * The task of the app's activities, as one task of Android's activity manager: a stack with the resumed activity on
 * top. Each step runs on the main thread, posted as the system's transactions are. When the last activity finishes,
 * the process ends.
 */
public final class Activities {
    private record Record(Activity activity, Activity caller, int requestCode) {}

    private static final ArrayList<Record> stack = new ArrayList<>();
    private static final Handler main = new Handler(Looper.getMainLooper());

    private Activities() {}

    private static Record top() { return stack.isEmpty() ? null : stack.get(stack.size() - 1); }
    private static Record record(Activity a) {
        for (Record r : stack) if (r.activity() == a) return r;
        return null;
    }

    /** An intent's activity in this app, or null for one that only another app (none here) could take. */
    private static ActivityInfo resolve(Intent intent) {
        ComponentName c = intent.getComponent();
        if (c != null) return Package.get().activity(c.getClassName());
        ResolveInfo r = PackageManagerImpl.get().resolveActivity(intent, 0);
        return r != null ? r.activityInfo : null;
    }

    public static void start(Activity caller, Intent intent, int requestCode, Bundle options) {
        ActivityInfo info = resolve(intent);
        if (info == null) throw new android.content.ActivityNotFoundException("No Activity found to handle " + intent);
        main.post(() -> launch(info, intent, caller, requestCode, null, null));
    }

    /** The first activity, from the launcher's intent. */
    public static void launchMain(String className) {
        ActivityInfo info = Package.get().activity(className);
        if (info == null) throw new IllegalStateException("the manifest has no activity " + className);
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setClassName(info.packageName, info.name)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        launch(info, intent, null, -1, null, null);
    }

    private static void launch(ActivityInfo info, Intent intent, Activity caller, int requestCode, Bundle saved, Object lastNonConfig) {
        String className = info.targetActivity != null ? info.targetActivity : info.name;
        Activity activity;
        try {
            activity = App.factory().instantiateActivity(App.classLoader(), className, intent);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException("Unable to instantiate activity " + className, e);
        }
        Record previous = top();
        if (previous != null && !previous.activity().isFinishing()) previous.activity().performPause();
        ContextImpl base = App.context(info.getThemeResource());
        activity.attach(base, App.application(), intent, info, new WindowManagerImpl(activity), lastNonConfig);
        base.setOuter(activity);
        stack.add(new Record(activity, caller, requestCode));
        activity.performCreate(saved);
        activity.performStart();
        if (saved != null) activity.performRestoreInstanceState(saved);
        activity.performPostCreate(saved);
        activity.performResume();
        activity.makeVisible();
        if (previous != null && !previous.activity().isFinishing()) {
            previous.activity().performSaveInstanceState(new Bundle());
            previous.activity().performStop();
        }
    }

    public static void finish(Activity activity) { main.post(() -> remove(activity)); }

    private static void remove(Activity activity) {
        Record r = record(activity);
        if (r == null) return;
        boolean wasTop = r == top();
        if (wasTop) activity.performPause();
        activity.performStop();
        activity.performDestroy();
        stack.remove(r);
        Record next = top();
        if (next == null) {
            snx.android.Main.exit(0);
            return;
        }
        if (r.caller() == next.activity() && r.requestCode() >= 0)
            next.activity().performActivityResult(r.requestCode(), activity.resultCode(), activity.resultData());
        if (wasTop) {
            next.activity().performStart();
            next.activity().performResume();
        }
    }

    /** The activity made again, as for a configuration change: its state saved, destroyed and launched anew. */
    public static void recreate(Activity activity) {
        main.post(() -> {
            Record r = record(activity);
            if (r == null) return;
            activity.setChangingConfigurations(true);
            Bundle state = new Bundle();
            activity.performPause();
            activity.performSaveInstanceState(state);
            activity.performStop();
            Object retained = activity.onRetainNonConfigurationInstance();
            activity.performDestroy();
            int index = stack.indexOf(r);
            stack.remove(index);
            launch(Package.get().activity(activity.getComponentName().getClassName()), activity.getIntent(), r.caller(), r.requestCode(), state, retained);
            // launch put it on top: the same place again
            Record made = stack.remove(stack.size() - 1);
            stack.add(index, made);
        });
    }

}
