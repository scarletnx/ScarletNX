// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.os.Parcel;
import android.text.ParcelableSpan;
import android.text.TextPaint;

/** Text with the baseline lowered by half the ascent. */
public class SubscriptSpan extends MetricAffectingSpan implements ParcelableSpan {
    public SubscriptSpan() {}
    public SubscriptSpan(Parcel src) {}
    @Override public int getSpanTypeId() { return 15; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {}
    @Override public void updateDrawState(TextPaint textPaint) { textPaint.baselineShift -= (int) (textPaint.ascent() / 2); }
    @Override public void updateMeasureState(TextPaint textPaint) { textPaint.baselineShift -= (int) (textPaint.ascent() / 2); }
}
