// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.accessibility;

import java.util.Locale;

/** The user's caption settings. Captions are off here, with the default style and scale. */
public class CaptioningManager {
    public CaptioningManager(android.content.Context context) {}
    public final boolean isEnabled() { return false; }
    public final Locale getLocale() { return null; }
    public final float getFontScale() { return 1; }
    public final boolean isSystemAudioCaptioningEnabled() { return false; }
    public final boolean isSystemAudioCaptioningUiEnabled() { return false; }
    public void addCaptioningChangeListener(CaptioningChangeListener listener) {}
    public void removeCaptioningChangeListener(CaptioningChangeListener listener) {}

    public abstract static class CaptioningChangeListener {
        public CaptioningChangeListener() {}
        public void onEnabledChanged(boolean enabled) {}
        public void onLocaleChanged(Locale locale) {}
        public void onFontScaleChanged(float fontScale) {}
        public void onSystemAudioCaptioningChanged(boolean enabled) {}
        public void onSystemAudioCaptioningUiChanged(boolean enabled) {}
    }
}
