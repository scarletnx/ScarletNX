// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;

/** A short message: written to the log, as there is no toast window yet. */
public class Toast {
    public static final int LENGTH_SHORT = 0, LENGTH_LONG = 1;
    private CharSequence text;
    private int duration;

    public Toast(Context context) {}
    public static Toast makeText(Context context, CharSequence text, int duration) { Toast t = new Toast(context); t.text = text; t.duration = duration; return t; }
    public static Toast makeText(Context context, int resId, int duration) { return makeText(context, context.getText(resId), duration); }
    public void show() { android.util.Log.i("Toast", String.valueOf(text)); }
    public void cancel() {}
    public void setText(CharSequence s) { text = s; }
    public void setText(int resId) {}
    public void setDuration(int duration) { this.duration = duration; }
    public int getDuration() { return duration; }
    public void setGravity(int gravity, int xOffset, int yOffset) {}
}
