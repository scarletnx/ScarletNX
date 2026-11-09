// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.job;

import java.util.ArrayList;
import java.util.List;

/**
 * Jobs: each runs on the main thread after its minimum latency, in its JobService, while the app is open. A namespace
 * (forNamespace) has jobs of its own: the same ID in two namespaces is two jobs.
 */
public abstract class JobScheduler {
    public static final int RESULT_FAILURE = 0, RESULT_SUCCESS = 1;
    private static final ArrayList<JobInfo> pending = new ArrayList<>();
    /* each pending job's namespace, null for the default one */
    private static final java.util.IdentityHashMap<JobInfo, String> namespaces = new java.util.IdentityHashMap<>();

    /** the scheduler of a namespace's jobs */
    public JobScheduler forNamespace(String namespace) {
        if (namespace == null) throw new NullPointerException("namespace cannot be null");
        if (namespace.isEmpty()) throw new IllegalArgumentException("namespace cannot be empty");
        return this;
    }
    public String getNamespace() { return null; }

    public abstract int schedule(JobInfo job);
    public abstract void cancel(int jobId);
    public abstract void cancelAll();
    public abstract List<JobInfo> getAllPendingJobs();
    public abstract JobInfo getPendingJob(int jobId);
    public int enqueue(JobInfo job, JobWorkItem work) { return schedule(job); }

    static void finished(JobParameters params) {
        synchronized (pending) {
            pending.removeIf(j -> j == params.info());
            namespaces.remove(params.info());
        }
    }

    /** The process's scheduler, of the default namespace or another. */
    public static final class Local extends JobScheduler {
        private final android.content.Context context;
        private final String namespace;
        private final android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());

        public Local(android.content.Context context) { this(context, null); }
        private Local(android.content.Context context, String namespace) {
            this.context = context;
            this.namespace = namespace;
        }
        @Override public JobScheduler forNamespace(String namespace) {
            super.forNamespace(namespace);
            return new Local(context, namespace);
        }
        @Override public String getNamespace() { return namespace; }
        private boolean mine(JobInfo j) { return java.util.Objects.equals(namespaces.get(j), namespace); }
        @Override public int schedule(JobInfo job) {
            cancel(job.id);
            synchronized (pending) {
                pending.add(job);
                namespaces.put(job, namespace);
            }
            handler.postDelayed(() -> run(job), job.minLatency);
            return RESULT_SUCCESS;
        }
        private void run(JobInfo job) {
            synchronized (pending) { if (!pending.contains(job)) return; }
            JobService service = (JobService) snx.android.app.Services.get(job.service);
            if (service == null || !service.onStartJob(new JobParameters(job))) finished(new JobParameters(job));
        }
        @Override public void cancel(int jobId) {
            synchronized (pending) {
                pending.removeIf(j -> {
                    boolean gone = j.id == jobId && mine(j);
                    if (gone) namespaces.remove(j);
                    return gone;
                });
            }
        }
        @Override public void cancelAll() {
            synchronized (pending) {
                pending.removeIf(j -> {
                    boolean gone = mine(j);
                    if (gone) namespaces.remove(j);
                    return gone;
                });
            }
        }
        @Override public List<JobInfo> getAllPendingJobs() {
            synchronized (pending) {
                ArrayList<JobInfo> out = new ArrayList<>();
                for (JobInfo j : pending) if (mine(j)) out.add(j);
                return out;
            }
        }
        @Override public JobInfo getPendingJob(int jobId) {
            synchronized (pending) { for (JobInfo j : pending) if (j.id == jobId && mine(j)) return j; }
            return null;
        }
    }
}
