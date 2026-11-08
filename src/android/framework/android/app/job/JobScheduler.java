// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.job;

import java.util.ArrayList;
import java.util.List;

/** Jobs: each runs on the main thread after its minimum latency, in its JobService, while the app is open. */
public abstract class JobScheduler {
    public static final int RESULT_FAILURE = 0, RESULT_SUCCESS = 1;
    private static final ArrayList<JobInfo> pending = new ArrayList<>();

    public abstract int schedule(JobInfo job);
    public abstract void cancel(int jobId);
    public abstract void cancelAll();
    public abstract List<JobInfo> getAllPendingJobs();
    public abstract JobInfo getPendingJob(int jobId);
    public int enqueue(JobInfo job, JobWorkItem work) { return schedule(job); }

    static void finished(JobParameters params) { synchronized (pending) { pending.removeIf(j -> j.id == params.getJobId()); } }

    /** The process's scheduler. */
    public static final class Local extends JobScheduler {
        private final android.content.Context context;
        private final android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());

        public Local(android.content.Context context) { this.context = context; }
        @Override public int schedule(JobInfo job) {
            cancel(job.id);
            synchronized (pending) { pending.add(job); }
            handler.postDelayed(() -> run(job), job.minLatency);
            return RESULT_SUCCESS;
        }
        private void run(JobInfo job) {
            synchronized (pending) { if (!pending.contains(job)) return; }
            JobService service = (JobService) snx.android.app.Services.get(job.service);
            if (service == null || !service.onStartJob(new JobParameters(job))) finished(new JobParameters(job));
        }
        @Override public void cancel(int jobId) { synchronized (pending) { pending.removeIf(j -> j.id == jobId); } }
        @Override public void cancelAll() { synchronized (pending) { pending.clear(); } }
        @Override public List<JobInfo> getAllPendingJobs() { synchronized (pending) { return new ArrayList<>(pending); } }
        @Override public JobInfo getPendingJob(int jobId) {
            synchronized (pending) { for (JobInfo j : pending) if (j.id == jobId) return j; }
            return null;
        }
    }
}
