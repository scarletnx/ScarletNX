// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics;

/** The rectangles of a region. A region here is one rectangle, so a non-empty region gives one. */
public class RegionIterator {
    private final Rect bounds = new Rect();
    private boolean done;

    public RegionIterator(Region region) { done = !region.getBounds(bounds); }
    public final boolean next(Rect r) {
        if (r == null) throw new NullPointerException("The Rect must be provided");
        if (done) return false;
        r.set(bounds);
        done = true;
        return true;
    }
}
