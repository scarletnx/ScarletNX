// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;

/** A seek bar that tells a listener when the user moves its thumb. */
public class SeekBar extends AbsSeekBar {
    private OnSeekBarChangeListener listener;

    public interface OnSeekBarChangeListener {
        void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser);
        void onStartTrackingTouch(SeekBar seekBar);
        void onStopTrackingTouch(SeekBar seekBar);
    }

    public SeekBar(Context context) { this(context, null); }
    public SeekBar(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public SeekBar(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public SeekBar(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public void setOnSeekBarChangeListener(OnSeekBarChangeListener l) { listener = l; }
    @Override void onProgressRefresh(boolean fromUser) { if (listener != null) listener.onProgressChanged(this, getProgress(), fromUser); }
    @Override void onStartTrackingTouch() { if (listener != null) listener.onStartTrackingTouch(this); }
    @Override void onStopTrackingTouch() { if (listener != null) listener.onStopTrackingTouch(this); }
    @Override public CharSequence getAccessibilityClassName() { return SeekBar.class.getName(); }
}
