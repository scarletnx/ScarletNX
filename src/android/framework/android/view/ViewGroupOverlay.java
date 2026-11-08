// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Canvas;
import java.util.ArrayList;

/**
 * A ViewGroup's overlay: drawables, and views drawn over its content where they are (a transition moves a view there
 * to animate it out). A view added here leaves its parent, as on Android.
 */
public class ViewGroupOverlay extends ViewOverlay {
    private final ArrayList<View> views = new ArrayList<>();

    ViewGroupOverlay(ViewGroup host) { super(host); }

    public void add(View view) {
        if (view == null) throw new IllegalArgumentException("view must be non-null");
        if (view.getParent() instanceof ViewGroup parent) parent.removeView(view);
        if (!views.contains(view)) views.add(view);
        host.invalidate();
    }
    public void remove(View view) {
        if (view == null) throw new IllegalArgumentException("view must be non-null");
        if (views.remove(view)) host.invalidate();
    }
    @Override public void clear() {
        views.clear();
        super.clear();
    }
    @Override void draw(Canvas canvas) {
        super.draw(canvas);
        for (View v : views) {
            int save = canvas.save();
            canvas.translate(v.getLeft(), v.getTop());
            v.draw(canvas);
            canvas.restoreToCount(save);
        }
    }
}
