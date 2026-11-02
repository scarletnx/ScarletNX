// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** Scroll capture of a view's content (long screenshots): never asked for here. */
public interface ScrollCaptureCallback {
    void onScrollCaptureSearch(android.os.CancellationSignal signal, java.util.function.Consumer<android.graphics.Rect> onReady);
    void onScrollCaptureStart(ScrollCaptureSession session, android.os.CancellationSignal signal, Runnable onReady);
    void onScrollCaptureImageRequest(ScrollCaptureSession session, android.os.CancellationSignal signal, android.graphics.Rect captureArea,
                                     java.util.function.Consumer<android.graphics.Rect> onComplete);
    void onScrollCaptureEnd(Runnable onReady);
}
