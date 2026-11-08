// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.speech;

import android.os.Bundle;

/** What a speech recognizer reports */
public interface RecognitionListener {
    void onReadyForSpeech(Bundle params);
    void onBeginningOfSpeech();
    void onRmsChanged(float rmsdB);
    void onBufferReceived(byte[] buffer);
    void onEndOfSpeech();
    void onError(int error);
    void onResults(Bundle results);
    void onPartialResults(Bundle partialResults);
    void onEvent(int eventType, Bundle params);
    default void onSegmentResults(Bundle segmentResults) {}
    default void onEndOfSegmentedSession() {}
    default void onLanguageDetection(Bundle results) {}
}
