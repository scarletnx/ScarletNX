// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

/** The answer to a fill request */
public final class FillCallback {
    FillCallback() {}
    public void onSuccess(FillResponse response) {}
    public void onFailure(CharSequence message) {}
}
