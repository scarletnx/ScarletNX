// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** Picture-in-picture options. */
public final class PictureInPictureParams {
    public static class Builder {
        public Builder setAspectRatio(android.util.Rational aspectRatio) { return this; }
        public Builder setSourceRectHint(android.graphics.Rect launchBounds) { return this; }
        public Builder setAutoEnterEnabled(boolean autoEnterEnabled) { return this; }
        public PictureInPictureParams build() { return new PictureInPictureParams(); }
    }
}
