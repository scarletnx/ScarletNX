// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.os.Handler;

/** A request for audio focus (AudioManager.requestAudioFocus): what it asks and who to tell when it changes. */
public final class AudioFocusRequest {
    private final int focusGain;
    private final AudioAttributes attributes;
    private final AudioManager.OnAudioFocusChangeListener listener;
    private final Handler handler;
    private final boolean delayed, pauses, locks;

    private AudioFocusRequest(Builder b) {
        focusGain = b.focusGain;
        attributes = b.attributes;
        listener = b.listener;
        handler = b.handler;
        delayed = b.delayed;
        pauses = b.pauses;
        locks = b.locks;
    }

    public int getFocusGain() { return focusGain; }
    public AudioAttributes getAudioAttributes() { return attributes; }
    public boolean acceptsDelayedFocusGain() { return delayed; }
    public boolean willPauseWhenDucked() { return pauses; }
    public boolean locksFocus() { return locks; }

    public static final class Builder {
        private int focusGain;
        private AudioAttributes attributes = new AudioAttributes.Builder().build();
        private AudioManager.OnAudioFocusChangeListener listener;
        private Handler handler;
        private boolean delayed, pauses, locks;

        public Builder(int focusGain) { setFocusGain(focusGain); }
        public Builder(AudioFocusRequest request) {
            focusGain = request.focusGain;
            attributes = request.attributes;
            listener = request.listener;
            handler = request.handler;
            delayed = request.delayed;
            pauses = request.pauses;
            locks = request.locks;
        }
        public Builder setFocusGain(int focusGain) {
            if (focusGain < AudioManager.AUDIOFOCUS_GAIN || focusGain > AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_EXCLUSIVE)
                throw new IllegalArgumentException("Illegal audio focus gain type " + focusGain);
            this.focusGain = focusGain;
            return this;
        }
        public Builder setOnAudioFocusChangeListener(AudioManager.OnAudioFocusChangeListener listener) {
            if (listener == null) throw new NullPointerException("Illegal null focus listener");
            this.listener = listener;
            handler = null;
            return this;
        }
        public Builder setOnAudioFocusChangeListener(AudioManager.OnAudioFocusChangeListener listener, Handler handler) {
            if (listener == null || handler == null) throw new NullPointerException("Illegal null focus listener or handler");
            this.listener = listener;
            this.handler = handler;
            return this;
        }
        public Builder setAudioAttributes(AudioAttributes attributes) {
            if (attributes == null) throw new NullPointerException("Illegal null AudioAttributes");
            this.attributes = attributes;
            return this;
        }
        public Builder setWillPauseWhenDucked(boolean pauseOnDuck) { pauses = pauseOnDuck; return this; }
        public Builder setAcceptsDelayedFocusGain(boolean acceptsDelayedFocusGain) { delayed = acceptsDelayedFocusGain; return this; }
        public Builder setForceDucking(boolean forceDucking) { return this; }
        public AudioFocusRequest build() {
            if ((delayed || pauses) && listener == null)
                throw new IllegalStateException("Can't use delayed focus or pause on duck without a listener");
            return new AudioFocusRequest(this);
        }
    }
}
