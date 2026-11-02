// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.contentcapture;

import android.view.View;
import android.view.ViewStructure;
import android.view.autofill.AutofillId;

/** Content capture: no service, so nothing is captured. */
public abstract class ContentCaptureSession implements AutoCloseable {
    public final AutofillId newAutofillId(AutofillId hostId, long virtualChildId) { return new AutofillId(hostId, (int) virtualChildId); }
    public final ViewStructure newViewStructure(View node) { return null; }
    public final ViewStructure newVirtualViewStructure(AutofillId parentId, long virtualId) { return null; }
    public final void notifyViewAppeared(ViewStructure node) {}
    public final void notifyViewDisappeared(AutofillId id) {}
    public final void notifyViewsDisappeared(AutofillId hostId, long[] virtualIds) {}
    public final void notifyViewTextChanged(AutofillId id, CharSequence text) {}
    public final void notifyViewInsetsChanged(android.graphics.Insets viewInsets) {}
    public void notifyViewsAppeared(java.util.List<ViewStructure> appearedNodes) {}
    public final void notifySessionPaused() {}
    public final void notifySessionResumed() {}
    public void close() {}
}
