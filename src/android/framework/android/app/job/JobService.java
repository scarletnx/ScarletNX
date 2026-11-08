// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.job;

import android.content.Intent;
import android.os.IBinder;

/** A service that runs jobs: the scheduler calls it in the process. */
public abstract class JobService extends android.app.Service {
    public static final String PERMISSION_BIND = "android.permission.BIND_JOB_SERVICE";
    public static final int JOB_END_NOTIFICATION_POLICY_DETACH = 0, JOB_END_NOTIFICATION_POLICY_REMOVE = 1;

    @Override public final IBinder onBind(Intent intent) { return null; }
    public abstract boolean onStartJob(JobParameters params);
    public abstract boolean onStopJob(JobParameters params);
    public void onNetworkChanged(JobParameters params) {}
    public final void jobFinished(JobParameters params, boolean wantsReschedule) { JobScheduler.finished(params); }
}
