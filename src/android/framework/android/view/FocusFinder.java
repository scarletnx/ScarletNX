// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Rect;
import java.util.ArrayList;

/** The next focusable view in a direction: the nearest one whose center lies that way. */
public class FocusFinder {
    private static final FocusFinder INSTANCE = new FocusFinder();

    public static FocusFinder getInstance() { return INSTANCE; }

    public final View findNextFocus(ViewGroup root, View focused, int direction) {
        ArrayList<View> all = new ArrayList<>();
        root.addFocusables(all, direction);
        if (focused == null) return all.isEmpty() ? null : all.get(0);
        int i = all.indexOf(focused);
        if (direction == View.FOCUS_FORWARD) return i + 1 < all.size() ? all.get(i + 1) : null;
        if (direction == View.FOCUS_BACKWARD) return i > 0 ? all.get(i - 1) : null;
        return findNextFocusFromRect(root, rect(focused), direction);
    }

    private static Rect rect(View v) {
        int[] loc = new int[2];
        v.getLocationInWindow(loc);
        return new Rect(loc[0], loc[1], loc[0] + v.getWidth(), loc[1] + v.getHeight());
    }

    public View findNextFocusFromRect(ViewGroup root, Rect focusedRect, int direction) {
        ArrayList<View> all = new ArrayList<>();
        root.addFocusables(all, direction);
        View best = null;
        long bestScore = Long.MAX_VALUE;
        for (View v : all) {
            Rect r = rect(v);
            int dx = r.centerX() - focusedRect.centerX(), dy = r.centerY() - focusedRect.centerY();
            boolean way = switch (direction) {
                case View.FOCUS_LEFT -> dx < 0;
                case View.FOCUS_RIGHT -> dx > 0;
                case View.FOCUS_UP -> dy < 0;
                case View.FOCUS_DOWN -> dy > 0;
                default -> false;
            };
            if (!way) continue;
            long score = (long) dx * dx + (long) dy * dy;
            if (score < bestScore) { bestScore = score; best = v; }
        }
        return best;
    }

    public View findNearestTouchable(ViewGroup root, int x, int y, int direction, int[] deltas) { return null; }
}
