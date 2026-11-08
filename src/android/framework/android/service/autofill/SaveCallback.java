// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

/** The answer to a save request */
public final class SaveCallback {
    SaveCallback() {}
    public void onSuccess() {}
    public void onSuccess(android.content.IntentSender intentSender) {}
    public void onFailure(CharSequence message) {}
}
