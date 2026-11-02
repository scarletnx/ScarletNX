// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.autofill;

import android.view.View;

/** Autofill: no service, so it is off. */
public final class AutofillManager {
    public abstract static class AutofillCallback {
        public static final int EVENT_INPUT_SHOWN = 1, EVENT_INPUT_HIDDEN = 2, EVENT_INPUT_UNAVAILABLE = 3;
        public void onAutofillEvent(View view, int event) {}
        public void onAutofillEvent(View view, int virtualId, int event) {}
    }

    public boolean isEnabled() { return false; }
    public boolean isAutofillSupported() { return false; }
    public boolean hasEnabledAutofillServices() { return false; }
    public void requestAutofill(View view) {}
    public void requestAutofill(View view, int virtualId, android.graphics.Rect absBounds) {}
    public void notifyViewEntered(View view) {}
    public void notifyViewEntered(View view, int virtualId, android.graphics.Rect absBounds) {}
    public void notifyViewExited(View view) {}
    public void notifyViewExited(View view, int virtualId) {}
    public void notifyValueChanged(View view) {}
    public void notifyValueChanged(View view, int virtualId, AutofillValue value) {}
    public void notifyViewVisibilityChanged(View view, boolean isVisible) {}
    public void notifyViewVisibilityChanged(View view, int virtualId, boolean isVisible) {}
    public void notifyViewClicked(View view) {}
    public void notifyViewClicked(View view, int virtualId) {}
    public void commit() {}
    public void cancel() {}
    public void registerCallback(AutofillCallback callback) {}
    public void unregisterCallback(AutofillCallback callback) {}
}
