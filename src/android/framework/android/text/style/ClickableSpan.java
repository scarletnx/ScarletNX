// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;
import android.view.View;

/** Text that does something when tapped. */
public abstract class ClickableSpan extends CharacterStyle implements UpdateAppearance {
    private static int nextId;
    private final int id = nextId++;

    public abstract void onClick(View widget);

    @Override public void updateDrawState(TextPaint ds) {
        ds.setColor(ds.linkColor);
        ds.setUnderlineText(true);
    }

    public int getId() { return id; }
}
