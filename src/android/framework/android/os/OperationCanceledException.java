// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2012 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.os;

/** An operation canceled by a CancellationSignal. */
public class OperationCanceledException extends RuntimeException {
    public OperationCanceledException() { this(null); }
    public OperationCanceledException(String message) { super(message != null ? message : "The operation has been canceled."); }
}
