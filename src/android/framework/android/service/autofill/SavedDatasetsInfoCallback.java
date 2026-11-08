// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

/** The answer to a request for what a service saved */
public interface SavedDatasetsInfoCallback {
    int ERROR_OTHER = 0, ERROR_UNSUPPORTED = 1, ERROR_NEEDS_USER_ACTION = 2;
    void onSuccess(java.util.Set<SavedDatasetsInfo> results);
    void onError(int error);
}
