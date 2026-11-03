// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

/** A drawable that animates. */
public interface Animatable {
    void start();

    void stop();

    boolean isRunning();
}
