// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.job;

import android.content.ComponentName;

/** A job's description. */
public class JobInfo implements android.os.Parcelable {
    public static final int NETWORK_TYPE_NONE = 0, NETWORK_TYPE_ANY = 1, NETWORK_TYPE_UNMETERED = 2, NETWORK_TYPE_NOT_ROAMING = 3,
        NETWORK_TYPE_CELLULAR = 4, NETWORK_TYPE_METERED = 4, BACKOFF_POLICY_LINEAR = 0, BACKOFF_POLICY_EXPONENTIAL = 1,
        PRIORITY_MIN = 100, PRIORITY_LOW = 200, PRIORITY_DEFAULT = 300, PRIORITY_HIGH = 400, PRIORITY_MAX = 500;
    public static final long DEFAULT_INITIAL_BACKOFF_MILLIS = 30000L, MAX_BACKOFF_DELAY_MILLIS = 18000000L;
    final int id;
    final ComponentName service;
    final android.os.PersistableBundle extras;
    final long minLatency, overrideDeadline;
    final boolean persisted;

    private JobInfo(Builder b) {
        id = b.id; service = b.service; extras = b.extras; minLatency = b.minLatency; overrideDeadline = b.overrideDeadline; persisted = b.persisted;
    }
    public int getId() { return id; }
    public ComponentName getService() { return service; }
    public android.os.PersistableBundle getExtras() { return extras; }
    public long getMinLatencyMillis() { return minLatency; }
    public long getMaxExecutionDelayMillis() { return overrideDeadline; }
    public boolean isPersisted() { return persisted; }
    public boolean isPeriodic() { return false; }
    public static final long getMinPeriodMillis() { return 900000; }
    public static final long getMinFlexMillis() { return 300000; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }

    public static final class TriggerContentUri {
        public static final int FLAG_NOTIFY_FOR_DESCENDANTS = 1;
        public TriggerContentUri(android.net.Uri uri, int flags) {}
    }

    public static final class Builder {
        private final int id;
        private final ComponentName service;
        private android.os.PersistableBundle extras = new android.os.PersistableBundle();
        private long minLatency, overrideDeadline;
        private boolean persisted;

        public Builder(int jobId, ComponentName jobService) { id = jobId; service = jobService; }
        public Builder setExtras(android.os.PersistableBundle extras) { this.extras = extras; return this; }
        public Builder setTransientExtras(android.os.Bundle extras) { return this; }
        public Builder setRequiredNetworkType(int networkType) { return this; }
        public Builder setRequiredNetwork(android.net.NetworkRequest networkRequest) { return this; }
        public Builder setRequiresCharging(boolean requiresCharging) { return this; }
        public Builder setRequiresBatteryNotLow(boolean batteryNotLow) { return this; }
        public Builder setRequiresDeviceIdle(boolean requiresDeviceIdle) { return this; }
        public Builder setRequiresStorageNotLow(boolean storageNotLow) { return this; }
        public Builder addTriggerContentUri(TriggerContentUri uri) { return this; }
        public Builder setTriggerContentUpdateDelay(long durationMs) { return this; }
        public Builder setTriggerContentMaxDelay(long durationMs) { return this; }
        public Builder setPeriodic(long intervalMillis) { return this; }
        public Builder setPeriodic(long intervalMillis, long flexMillis) { return this; }
        public Builder setMinimumLatency(long minLatencyMillis) { minLatency = minLatencyMillis; return this; }
        public Builder setOverrideDeadline(long maxExecutionDelayMillis) { overrideDeadline = maxExecutionDelayMillis; return this; }
        public Builder setBackoffCriteria(long initialBackoffMillis, int backoffPolicy) { return this; }
        public Builder setExpedited(boolean expedited) { return this; }
        public Builder setImportantWhileForeground(boolean importantWhileForeground) { return this; }
        public Builder setPrefetch(boolean prefetch) { return this; }
        public Builder setPersisted(boolean isPersisted) { persisted = isPersisted; return this; }
        public Builder setPriority(int priority) { return this; }
        public Builder setTraceTag(String tag) { return this; }
        public JobInfo build() { return new JobInfo(this); }
    }
}
