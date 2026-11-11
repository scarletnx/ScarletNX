// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics.drawable;

import android.graphics.drawable.shapes.RoundRectShape;
import java.util.Arrays;

/** A ShapeDrawable of a color whose corners may be rounded: a rectangle without them, a RoundRectShape with them. */
public class PaintDrawable extends ShapeDrawable {
    public PaintDrawable() {}
    public PaintDrawable(int color) { getPaint().setColor(color); }

    /** The same radius at every corner; 0 or less for square ones. */
    public void setCornerRadius(float radius) {
        float[] radii = null;
        if (radius > 0) {
            radii = new float[8];
            Arrays.fill(radii, radius);
        }
        setCornerRadii(radii);
    }

    /** The corners' radii, x and y of each from the top left clockwise; null for square ones. */
    public void setCornerRadii(float[] radii) {
        if (radii == null) {
            if (getShape() != null) setShape(null);
        } else {
            setShape(new RoundRectShape(radii, null, null));
        }
        invalidateSelf();
    }
}
