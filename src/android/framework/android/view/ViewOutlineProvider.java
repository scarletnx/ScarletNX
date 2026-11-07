// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2014 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view;

import android.graphics.Outline;

/** A view's outline: its background's, its bounds, or its padded bounds. */
public abstract class ViewOutlineProvider {
    public static final ViewOutlineProvider BACKGROUND = new ViewOutlineProvider() {
        @Override public void getOutline(View view, Outline outline) {
            android.graphics.drawable.Drawable background = view.getBackground();
            if (background != null) background.getOutline(outline);
            else { outline.setRect(0, 0, view.getWidth(), view.getHeight()); outline.setAlpha(0); }
        }
    };
    public static final ViewOutlineProvider BOUNDS = new ViewOutlineProvider() {
        @Override public void getOutline(View view, Outline outline) { outline.setRect(0, 0, view.getWidth(), view.getHeight()); }
    };
    public static final ViewOutlineProvider PADDED_BOUNDS = new ViewOutlineProvider() {
        @Override public void getOutline(View view, Outline outline) {
            outline.setRect(view.getPaddingLeft(), view.getPaddingTop(), view.getWidth() - view.getPaddingRight(), view.getHeight() - view.getPaddingBottom());
        }
    };

    public abstract void getOutline(View view, Outline outline);
}
