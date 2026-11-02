// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.inputmethod;

/** A stylus gesture at a point of text. */
public final class InsertGesture extends HandwritingGesture {
    public String getTextToInsert() { return ""; }
    public android.graphics.PointF getInsertionPoint() { return new android.graphics.PointF(); }
    public android.graphics.PointF getJoinOrSplitPoint() { return new android.graphics.PointF(); }
    public android.graphics.PointF getStartPoint() { return new android.graphics.PointF(); }
    public android.graphics.PointF getEndPoint() { return new android.graphics.PointF(); }
}
