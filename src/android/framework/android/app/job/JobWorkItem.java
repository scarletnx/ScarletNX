// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.job;

/** A unit of work of a job. */
public final class JobWorkItem {
    private final android.content.Intent intent;
    public JobWorkItem(android.content.Intent intent) { this.intent = intent; }
    public android.content.Intent getIntent() { return intent; }
}
