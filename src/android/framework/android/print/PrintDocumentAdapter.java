// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.print;

import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;

/** The content of a print job, which the app lays out and writes when the print system asks. */
public abstract class PrintDocumentAdapter {
    public static final String EXTRA_PRINT_PREVIEW = "EXTRA_PRINT_PREVIEW";

    public PrintDocumentAdapter() {}
    public void onStart() {}
    public abstract void onLayout(PrintAttributes oldAttributes, PrintAttributes newAttributes, CancellationSignal cancellationSignal,
        LayoutResultCallback callback, Bundle extras);
    public abstract void onWrite(PageRange[] pages, ParcelFileDescriptor destination, CancellationSignal cancellationSignal,
        WriteResultCallback callback);
    public void onFinish() {}

    /** The end of a layout. */
    public static abstract class LayoutResultCallback {
        public LayoutResultCallback() {}
        public void onLayoutFinished(PrintDocumentInfo info, boolean changed) {}
        public void onLayoutFailed(CharSequence error) {}
        public void onLayoutCancelled() {}
    }

    /** The end of a write. */
    public static abstract class WriteResultCallback {
        public WriteResultCallback() {}
        public void onWriteFinished(PageRange[] pages) {}
        public void onWriteFailed(CharSequence error) {}
        public void onWriteCancelled() {}
    }
}
