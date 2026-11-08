// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.speech;

/** A recognizer's answer to checkRecognitionSupport */
public interface RecognitionSupportCallback {
    void onSupportResult(RecognitionSupport recognitionSupport);
    void onError(int error);
}
