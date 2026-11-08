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
    /* activities below the top that a configuration change needs made again: they are when they come back on top,
       as Android relaunches an activity that is not visible when it is next shown, in its place in the task */
    private static final java.util.Set<Activity> relaunchPending = java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<>());
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

    /** The system's pickers, which every device has: here none, as a picker the user closed */
    private static final java.util.Set<String> PICKERS = java.util.Set.of(Intent.ACTION_OPEN_DOCUMENT,
        Intent.ACTION_OPEN_DOCUMENT_TREE, Intent.ACTION_CREATE_DOCUMENT, Intent.ACTION_GET_CONTENT, Intent.ACTION_PICK);

    /**
     * The picker's answer for a run without a user (--pick=PATH, snx.android.pick): a tree of the directory (the
     * path's own, or its parent's) for OPEN_DOCUMENT_TREE, the file for the other pickers, as documents of the
     * shared storage's provider; null (none picked) for none, or for a file a creating picker asks for
     */
    private static Intent pick(Intent request) {
        String path = System.getProperty("snx.android.pick");
        if (path == null || path.isEmpty() || Intent.ACTION_CREATE_DOCUMENT.equals(request.getAction())) return null;
        java.io.File f = new java.io.File(path).getAbsoluteFile();
        if (!f.exists()) return null;
        String authority = snx.android.provider.ExternalStorageProvider.AUTHORITY;
        android.net.Uri uri;
        boolean tree = Intent.ACTION_OPEN_DOCUMENT_TREE.equals(request.getAction());
        if (!tree && f.isDirectory()) return null;
        // the folder (or the file's folder) as a folder of the shared storage
        java.io.File folder = snx.android.provider.ExternalStorageProvider.shared(f.isDirectory() ? f : f.getParentFile());
        if (folder == null) return null;
        if (tree) {
            uri = android.provider.DocumentsContract.buildTreeDocumentUri(authority, snx.android.provider.ExternalStorageProvider.documentId(folder));
        } else {
            uri = android.provider.DocumentsContract.buildDocumentUri(authority,
                snx.android.provider.ExternalStorageProvider.documentId(new java.io.File(folder, f.getName())));
        }
        return new Intent().setData(uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
    }

    public static void start(Activity caller, Intent intent, int requestCode, Bundle options) {
        ActivityInfo info = resolve(intent);
        if (info == null && PICKERS.contains(intent.getAction())) {
            // the scripted pick (--pick) as DocumentsUI's answer, else RESULT_CANCELED, as Android delivers it when
            // the user backs out of the picker
            Intent result = pick(intent);
            if (caller != null && requestCode >= 0)
                main.post(() -> caller.performActivityResult(requestCode, result != null ? Activity.RESULT_OK : Activity.RESULT_CANCELED, result));
            return;
        }
        if (info == null && intent.getAction() != null && (intent.getAction().startsWith("android.settings.")
                || intent.getAction().equals(android.app.role.RoleManager.ACTION_REQUEST_ROLE))) {
            // a screen of the system's Settings, which every device has, or its dialog asking for a role: shown and
            // backed out of, RESULT_CANCELED, as Settings gives (what it would grant, the app has: the permissions it
            // asks for)
            if (caller != null && requestCode >= 0)
                main.post(() -> caller.performActivityResult(requestCode, Activity.RESULT_CANCELED, null));
            return;
        }
        if (info == null) throw new android.content.ActivityNotFoundException("No Activity found to handle " + intent);
        main.post(() -> launch(info, intent, caller, requestCode, null, null, false));
    }

    /** The first activity, from the launcher's intent. */
    public static void launchMain(String className) {
        ActivityInfo info = Package.get().activity(className);
        if (info == null) throw new IllegalStateException("the manifest has no activity " + className);
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setClassName(info.packageName, info.name)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        launch(info, intent, null, -1, null, null, false);
    }

    /** an activity created and resumed on top; a relaunch (recreate) leaves the activity below it as it is */
    private static void launch(ActivityInfo info, Intent intent, Activity caller, int requestCode, Bundle saved, Object lastNonConfig,
                               boolean relaunch) {
        String className = info.targetActivity != null ? info.targetActivity : info.name;
        /* the manifest's orientation: Android turns the display before the activity starts */
        snx.android.Device.rotate(info.screenOrientation);
        Activity activity;
        try {
            activity = App.factory().instantiateActivity(App.classLoader(), className, intent);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException("Unable to instantiate activity " + className, e);
        }
        Record previous = relaunch ? null : top();
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

    /** Every activity finished, from the top down, as the window's close does: the process then ends */
    public static void finishAll() {
        for (int i = stack.size() - 1; i >= 0; --i) stack.get(i).activity().finish();
    }

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
            if (relaunchPending.remove(next.activity())) {
                recreate(next.activity());
            } else {
                next.activity().performStart();
                next.activity().performResume();
            }
        }
    }

    /**
     * A change of the device's configuration (the display turned or resized), on the main thread as Android's
     * ActivityThread gives it: the application's callbacks first, then each activity. An activity whose manifest
     * names every changed field in android:configChanges gets onConfigurationChanged, with its views; another one is
     * made again (recreate), as Android relaunches it.
     */
    public static void configurationChanged(android.content.res.Configuration config) {
        main.post(() -> {
            android.app.Application app = App.application();
            if (app != null) app.onConfigurationChanged(new android.content.res.Configuration(config));
            for (Record r : new ArrayList<>(stack)) {
                Activity a = r.activity();
                if (a.isFinishing()) continue;
                android.content.res.Configuration old = a.appliedConfiguration();
                int diff = old.diff(config);
                if (diff == 0) continue;
                if ((diff & ~a.activityInfo().configChanges) == 0) {
                    a.applyConfiguration(config);
                    a.onConfigurationChanged(new android.content.res.Configuration(config));
                    android.view.View decor = a.getWindow().peekDecorView();
                    if (decor != null) decor.dispatchConfigurationChanged(new android.content.res.Configuration(config));
                } else if (r == top()) {
                    a.applyConfiguration(config);
                    recreate(a);
                } else {
                    relaunchPending.add(a);
                }
            }
        });
    }

    /** The activity made again, as for a configuration change: its state saved, destroyed and launched anew. */
    public static void recreate(Activity activity) {
        main.post(() -> {
            Record r = record(activity);
            if (r == null) return;
            relaunchPending.remove(activity);
            activity.setChangingConfigurations(true);
            Bundle state = new Bundle();
            // a stopped one (below the top until now) only saves its state again
            if (!activity.stoppedState()) {
                if (activity.isResumed()) activity.performPause();
                activity.performSaveInstanceState(state);
                activity.performStop();
            } else {
                activity.performSaveInstanceState(state);
            }
            Object retained = activity.onRetainNonConfigurationInstance();
            activity.performDestroy();
            int index = stack.indexOf(r);
            stack.remove(index);
            launch(Package.get().activity(activity.getComponentName().getClassName()), activity.getIntent(), r.caller(), r.requestCode(), state, retained, true);
            // launch put it on top: the same place again
            Record made = stack.remove(stack.size() - 1);
            stack.add(index, made);
        });
    }

}
