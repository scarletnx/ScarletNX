// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.job;

import android.app.Service;
import android.os.IBinder;

/** The job interface of a service that is not a JobService: the binder that its onBind() returns, and its callbacks. */
public abstract class JobServiceEngine {
    private final IBinder binder = new android.os.Binder();

    public JobServiceEngine(Service service) {
        if (service == null) throw new NullPointerException("service");
    }
    public final IBinder getBinder() { return binder; }
    public abstract boolean onStartJob(JobParameters params);
    public abstract boolean onStopJob(JobParameters params);
    public void onNetworkChanged(JobParameters params) {}
    public void jobFinished(JobParameters params, boolean wantsReschedule) {
        if (params == null) throw new NullPointerException("params");
        JobScheduler.finished(params);
    }
}
