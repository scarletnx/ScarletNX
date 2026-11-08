// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.speech;

import android.content.Context;
import android.content.Intent;
import java.util.concurrent.Executor;

/**
 * Speech recognition (Off in docs/android/INTERFACES.md): the device has no recognition service, so it is not
 * available, and a recognizer that listens reports ERROR_CLIENT, as Android's does without a service.
 */
public class SpeechRecognizer {
    public static final String RESULTS_RECOGNITION = "results_recognition", CONFIDENCE_SCORES = "confidence_scores";
    public static final int ERROR_NETWORK_TIMEOUT = 1, ERROR_NETWORK = 2, ERROR_AUDIO = 3, ERROR_SERVER = 4,
        ERROR_CLIENT = 5, ERROR_SPEECH_TIMEOUT = 6, ERROR_NO_MATCH = 7, ERROR_RECOGNIZER_BUSY = 8,
        ERROR_INSUFFICIENT_PERMISSIONS = 9, ERROR_TOO_MANY_REQUESTS = 10, ERROR_SERVER_DISCONNECTED = 11,
        ERROR_LANGUAGE_NOT_SUPPORTED = 12, ERROR_LANGUAGE_UNAVAILABLE = 13, ERROR_CANNOT_CHECK_SUPPORT = 14;

    private final android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper());
    private RecognitionListener listener;

    private SpeechRecognizer() {}

    public static boolean isRecognitionAvailable(Context context) { return false; }
    public static boolean isOnDeviceRecognitionAvailable(Context context) { return false; }
    public static SpeechRecognizer createSpeechRecognizer(Context context) { return new SpeechRecognizer(); }
    public static SpeechRecognizer createSpeechRecognizer(Context context, android.content.ComponentName serviceComponent) { return new SpeechRecognizer(); }
    public static SpeechRecognizer createOnDeviceSpeechRecognizer(Context context) { return new SpeechRecognizer(); }

    public void setRecognitionListener(RecognitionListener listener) { this.listener = listener; }
    public void startListening(Intent recognizerIntent) {
        RecognitionListener l = listener;
        if (l != null) main.post(() -> l.onError(ERROR_CLIENT));
    }
    public void stopListening() {}
    public void cancel() {}
    public void destroy() { listener = null; }
    public void checkRecognitionSupport(Intent recognizerIntent, Executor executor, RecognitionSupportCallback supportListener) {
        executor.execute(() -> supportListener.onError(ERROR_CANNOT_CHECK_SUPPORT));
    }
    public void triggerModelDownload(Intent recognizerIntent) {}
}
