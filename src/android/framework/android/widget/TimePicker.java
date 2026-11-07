// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import java.util.Calendar;

/** A time of day that the user picks. It starts at the time now; the runtime does not draw its clock or spinners. */
public class TimePicker extends FrameLayout {
    private int hour, minute;
    private boolean is24Hour = android.text.format.DateFormat.is24HourFormat(getContext());
    private OnTimeChangedListener listener;

    public interface OnTimeChangedListener { void onTimeChanged(TimePicker view, int hourOfDay, int minute); }

    public TimePicker(Context context) { this(context, null); }
    public TimePicker(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public TimePicker(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public TimePicker(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        Calendar c = Calendar.getInstance();
        hour = c.get(Calendar.HOUR_OF_DAY);
        minute = c.get(Calendar.MINUTE);
    }

    public void setHour(int hour) { if (hour >= 0 && hour <= 23 && hour != this.hour) { this.hour = hour; changed(); } }
    public int getHour() { return hour; }
    public void setMinute(int minute) { if (minute >= 0 && minute <= 59 && minute != this.minute) { this.minute = minute; changed(); } }
    public int getMinute() { return minute; }
    @Deprecated public void setCurrentHour(Integer currentHour) { setHour(currentHour); }
    @Deprecated public Integer getCurrentHour() { return hour; }
    @Deprecated public void setCurrentMinute(Integer currentMinute) { setMinute(currentMinute); }
    @Deprecated public Integer getCurrentMinute() { return minute; }
    public void setIs24HourView(Boolean is24HourView) { if (is24HourView != null) is24Hour = is24HourView; }
    public boolean is24HourView() { return is24Hour; }
    public void setOnTimeChangedListener(OnTimeChangedListener onTimeChangedListener) { listener = onTimeChangedListener; }
    private void changed() { if (listener != null) listener.onTimeChanged(this, hour, minute); }
    @Override public CharSequence getAccessibilityClassName() { return TimePicker.class.getName(); }
}
