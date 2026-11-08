// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.transition;

import android.view.Gravity;

/** Slides views in or out from an edge; here they show or hide at once. */
public class Slide extends Visibility {
    private int slideEdge = Gravity.BOTTOM;

    public Slide() {}
    public Slide(int slideEdge) { setSlideEdge(slideEdge); }
    public Slide(android.content.Context context, android.util.AttributeSet attrs) { super(context, attrs); }
    public void setSlideEdge(int slideEdge) {
        switch (slideEdge) {
            case Gravity.LEFT, Gravity.TOP, Gravity.RIGHT, Gravity.BOTTOM, Gravity.START, Gravity.END -> this.slideEdge = slideEdge;
            default -> throw new IllegalArgumentException("Invalid slide direction");
        }
    }
    public int getSlideEdge() { return slideEdge; }
}
