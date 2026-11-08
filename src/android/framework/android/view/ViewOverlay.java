// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import java.util.ArrayList;

/** Drawables drawn over a view after its content, in their own bounds, as Android's overlay. */
public class ViewOverlay {
    final View host;
    final ArrayList<Drawable> drawables = new ArrayList<>();

    ViewOverlay(View host) { this.host = host; }

    public void add(Drawable drawable) {
        if (drawable == null) throw new IllegalArgumentException("drawable must be non-null");
        if (!drawables.contains(drawable)) {
            drawables.add(drawable);
            drawable.setCallback(host);
            host.invalidate();
        }
    }
    public void remove(Drawable drawable) {
        if (drawable == null) throw new IllegalArgumentException("drawable must be non-null");
        if (drawables.remove(drawable)) {
            drawable.setCallback(null);
            host.invalidate();
        }
    }
    public void clear() {
        for (Drawable d : drawables) d.setCallback(null);
        drawables.clear();
        host.invalidate();
    }
    void draw(Canvas canvas) {
        for (Drawable d : drawables) d.draw(canvas);
    }
}
