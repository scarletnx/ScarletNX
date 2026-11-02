// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.job;

/** A running job's parameters. */
public class JobParameters implements android.os.Parcelable {
    public static final int STOP_REASON_UNDEFINED = 0, STOP_REASON_CANCELLED_BY_APP = 1;
    private final JobInfo info;
    JobParameters(JobInfo info) { this.info = info; }
    public int getJobId() { return info.id; }
    public android.os.PersistableBundle getExtras() { return info.extras; }
    public android.os.Bundle getTransientExtras() { return new android.os.Bundle(); }
    public android.net.Network getNetwork() { return null; }
    public android.net.Uri[] getTriggeredContentUris() { return null; }
    public String[] getTriggeredContentAuthorities() { return null; }
    public int getStopReason() { return STOP_REASON_UNDEFINED; }
    public boolean isOverrideDeadlineExpired() { return false; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
