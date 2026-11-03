// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.Context;

/** A dialog with a message and a progress value (deprecated): the message shows, the value is kept. */
@Deprecated
public class ProgressDialog extends AlertDialog {
    public static final int STYLE_SPINNER = 0, STYLE_HORIZONTAL = 1;
    private int progress, secondaryProgress, max = 100;
    private boolean indeterminate;

    public ProgressDialog(Context context) { super(context); }
    public ProgressDialog(Context context, int theme) { super(context, theme); }

    public static ProgressDialog show(Context context, CharSequence title, CharSequence message) { return show(context, title, message, false); }
    public static ProgressDialog show(Context context, CharSequence title, CharSequence message, boolean indeterminate) {
        return show(context, title, message, indeterminate, false, null);
    }
    public static ProgressDialog show(Context context, CharSequence title, CharSequence message, boolean indeterminate, boolean cancelable) {
        return show(context, title, message, indeterminate, cancelable, null);
    }
    public static ProgressDialog show(Context context, CharSequence title, CharSequence message, boolean indeterminate, boolean cancelable,
                                      OnCancelListener cancelListener) {
        ProgressDialog d = new ProgressDialog(context);
        d.setTitle(title);
        d.setMessage(message);
        d.setIndeterminate(indeterminate);
        d.setCancelable(cancelable);
        d.setOnCancelListener(cancelListener);
        d.show();
        return d;
    }

    @Override public void setMessage(CharSequence message) { super.setMessage(message); }
    public void setProgress(int value) { progress = Math.max(0, Math.min(max, value)); }
    public int getProgress() { return progress; }
    public void setSecondaryProgress(int secondaryProgress) { this.secondaryProgress = secondaryProgress; }
    public int getSecondaryProgress() { return secondaryProgress; }
    public void setMax(int max) { this.max = max; }
    public int getMax() { return max; }
    public void incrementProgressBy(int diff) { setProgress(progress + diff); }
    public void incrementSecondaryProgressBy(int diff) { secondaryProgress += diff; }
    public void setIndeterminate(boolean indeterminate) { this.indeterminate = indeterminate; }
    public boolean isIndeterminate() { return indeterminate; }
    public void setProgressStyle(int style) {}
    public void setProgressNumberFormat(String format) {}
    public void setProgressPercentFormat(java.text.NumberFormat format) {}
    public void setProgressDrawable(android.graphics.drawable.Drawable d) {}
    public void setIndeterminateDrawable(android.graphics.drawable.Drawable d) {}
}
