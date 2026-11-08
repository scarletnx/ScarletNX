// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.os.Parcel;
import android.text.ParcelableSpan;
import android.text.TextPaint;

/** Text with the baseline raised by half the ascent. */
public class SuperscriptSpan extends MetricAffectingSpan implements ParcelableSpan {
    public SuperscriptSpan() {}
    public SuperscriptSpan(Parcel src) {}
    @Override public int getSpanTypeId() { return 14; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {}
    @Override public void updateDrawState(TextPaint textPaint) { textPaint.baselineShift += (int) (textPaint.ascent() / 2); }
    @Override public void updateMeasureState(TextPaint textPaint) { textPaint.baselineShift += (int) (textPaint.ascent() / 2); }
}
