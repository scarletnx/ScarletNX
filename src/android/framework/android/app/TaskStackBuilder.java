// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import java.util.ArrayList;

/** A back stack of activities to start at once: an activity's parents (android:parentActivityName) before it */
public class TaskStackBuilder {
    private final ArrayList<Intent> intents = new ArrayList<>();
    private final Context source;

    private TaskStackBuilder(Context source) { this.source = source; }
    public static TaskStackBuilder create(Context context) { return new TaskStackBuilder(context); }

    public TaskStackBuilder addNextIntent(Intent nextIntent) {
        intents.add(nextIntent);
        return this;
    }
    public TaskStackBuilder addNextIntentWithParentStack(Intent nextIntent) {
        ComponentName target = nextIntent.getComponent();
        if (target == null) target = nextIntent.resolveActivity(source.getPackageManager());
        if (target != null) addParentStack(target);
        addNextIntent(nextIntent);
        return this;
    }
    public TaskStackBuilder addParentStack(Activity sourceActivity) { return addParentStack(sourceActivity.getComponentName()); }
    public TaskStackBuilder addParentStack(Class<?> sourceActivityClass) {
        return addParentStack(new ComponentName(source, sourceActivityClass));
    }
    /** the activity's parents, the farthest first, before the intents added after them */
    public TaskStackBuilder addParentStack(ComponentName sourceActivityName) {
        int insertAt = intents.size();
        PackageManager pm = source.getPackageManager();
        try {
            ActivityInfo info = pm.getActivityInfo(sourceActivityName, 0);
            while (info != null && info.parentActivityName != null) {
                ComponentName parent = new ComponentName(info.packageName, info.parentActivityName);
                intents.add(insertAt, new Intent().setComponent(parent));
                info = pm.getActivityInfo(parent, 0);
            }
        } catch (PackageManager.NameNotFoundException e) {
            throw new IllegalArgumentException(e);
        }
        return this;
    }
    public int getIntentCount() { return intents.size(); }
    @Deprecated public Intent getIntent(int index) { return editIntentAt(index); }
    public Intent editIntentAt(int index) { return intents.get(index); }
    /** the intents, the first one starting a new task as Android's does */
    public Intent[] getIntents() {
        Intent[] out = new Intent[intents.size()];
        if (out.length == 0) return out;
        out[0] = new Intent(intents.get(0)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_ACTIVITY_TASK_ON_HOME);
        for (int i = 1; i < out.length; ++i) out[i] = new Intent(intents.get(i));
        return out;
    }
    public void startActivities() { startActivities(null); }
    public void startActivities(android.os.Bundle options) {
        if (intents.isEmpty()) throw new IllegalStateException("No intents added to TaskStackBuilder; cannot startActivities");
        source.startActivities(getIntents(), options);
    }
    public PendingIntent getPendingIntent(int requestCode, int flags) { return getPendingIntent(requestCode, flags, null); }
    public PendingIntent getPendingIntent(int requestCode, int flags, android.os.Bundle options) {
        if (intents.isEmpty()) throw new IllegalStateException("No intents added to TaskStackBuilder; cannot getPendingIntent");
        return PendingIntent.getActivities(source, requestCode, getIntents(), flags, options);
    }
}
