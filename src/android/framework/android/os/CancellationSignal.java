// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A request to cancel an operation in progress. */
public final class CancellationSignal {
    public interface OnCancelListener {
        void onCancel();
    }

    private boolean canceled;
    private OnCancelListener listener;

    public boolean isCanceled() { synchronized (this) { return canceled; } }

    public void throwIfCanceled() {
        if (isCanceled()) throw new OperationCanceledException();
    }

    public void cancel() {
        OnCancelListener l;
        synchronized (this) {
            if (canceled) return;
            canceled = true;
            l = listener;
        }
        if (l != null) l.onCancel();
    }

    public void setOnCancelListener(OnCancelListener l) {
        synchronized (this) {
            if (listener == l) return;
            listener = l;
            if (!canceled || l == null) return;
        }
        l.onCancel();
    }
}
