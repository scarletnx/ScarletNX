// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics.drawable.shapes;

import android.graphics.Canvas;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.RectF;

/** A rectangle. */
public class RectShape extends Shape {
    private RectF rect = new RectF();

    public RectShape() {}
    @Override public void draw(Canvas canvas, Paint paint) { canvas.drawRect(rect, paint); }
    @Override public void getOutline(Outline outline) {
        outline.setRect((int) Math.ceil(rect.left), (int) Math.ceil(rect.top), (int) Math.floor(rect.right), (int) Math.floor(rect.bottom));
    }
    @Override protected void onResize(float width, float height) { rect.set(0, 0, width, height); }
    protected final RectF rect() { return rect; }
    @Override public RectShape clone() throws CloneNotSupportedException {
        RectShape s = (RectShape) super.clone();
        s.rect = new RectF(rect);
        return s;
    }
}
