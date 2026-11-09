// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.Context;
import android.view.Display;

/**
 * A dialog on another display (a secondary screen's content), as Android's: its context is the display's, so its
 * window goes to that display. A virtual display here shows nowhere, so the window lays out and draws for no screen.
 */
public class Presentation extends Dialog {
    private final Display display;

    public Presentation(Context outerContext, Display display) { this(outerContext, display, 0); }
    public Presentation(Context outerContext, Display display, int theme) {
        super(outerContext.createDisplayContext(display), theme);
        this.display = display;
        getWindow().getAttributes().width = android.view.ViewGroup.LayoutParams.MATCH_PARENT;
        getWindow().getAttributes().height = android.view.ViewGroup.LayoutParams.MATCH_PARENT;
    }
    public Presentation(Context outerContext, Display display, int theme, int type) { this(outerContext, display, theme); }

    public Display getDisplay() { return display; }
    public android.content.res.Resources getResources() { return getContext().getResources(); }
    public void onDisplayRemoved() {}
    public void onDisplayChanged() {}
    @Override public void show() {
        if (!display.isValid()) throw new android.view.WindowManager.InvalidDisplayException("Presentation's display is no longer valid");
        super.show();
    }
}
