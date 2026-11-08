// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view.animation;

/** A rate that starts and ends slowly. */
public class AccelerateDecelerateInterpolator extends BaseInterpolator {
    @Override public float getInterpolation(float input) { return (float) (Math.cos((input + 1) * Math.PI) / 2) + 0.5f; }
}
