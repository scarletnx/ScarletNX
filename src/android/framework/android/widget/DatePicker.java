// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import java.util.Calendar;

/** A date that the user picks. It starts at the date of today; the runtime does not draw its calendar or spinners. */
public class DatePicker extends FrameLayout {
    private int year, month, day;
    private long minDate = Long.MIN_VALUE, maxDate = Long.MAX_VALUE;
    private int firstDayOfWeek = Calendar.getInstance().getFirstDayOfWeek();
    private boolean calendarShown = true, spinnersShown = true;
    private OnDateChangedListener listener;

    public interface OnDateChangedListener { void onDateChanged(DatePicker view, int year, int monthOfYear, int dayOfMonth); }

    public DatePicker(Context context) { this(context, null); }
    public DatePicker(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public DatePicker(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public DatePicker(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        Calendar c = Calendar.getInstance();
        year = c.get(Calendar.YEAR);
        month = c.get(Calendar.MONTH);
        day = c.get(Calendar.DAY_OF_MONTH);
    }

    public void init(int year, int monthOfYear, int dayOfMonth, OnDateChangedListener onDateChangedListener) {
        set(year, monthOfYear, dayOfMonth);
        listener = onDateChangedListener;
    }
    public void setOnDateChangedListener(OnDateChangedListener onDateChangedListener) { listener = onDateChangedListener; }
    public void updateDate(int year, int month, int dayOfMonth) {
        if (set(year, month, dayOfMonth) && listener != null) listener.onDateChanged(this, this.year, this.month, day);
    }
    /** Sets the date, kept between the minimum and maximum dates. It returns true when the date changed. */
    private boolean set(int y, int m, int d) {
        Calendar c = Calendar.getInstance();
        c.clear();
        c.set(y, m, d);
        long t = Math.max(minDate, Math.min(maxDate, c.getTimeInMillis()));
        c.setTimeInMillis(t);
        int ny = c.get(Calendar.YEAR), nm = c.get(Calendar.MONTH), nd = c.get(Calendar.DAY_OF_MONTH);
        if (ny == year && nm == month && nd == day) return false;
        year = ny; month = nm; day = nd;
        return true;
    }
    public int getYear() { return year; }
    public int getMonth() { return month; }
    public int getDayOfMonth() { return day; }
    public long getMinDate() { return minDate == Long.MIN_VALUE ? new java.util.GregorianCalendar(1900, 0, 1).getTimeInMillis() : minDate; }
    public void setMinDate(long minDate) { this.minDate = minDate; set(year, month, day); }
    public long getMaxDate() { return maxDate == Long.MAX_VALUE ? new java.util.GregorianCalendar(2100, 11, 31).getTimeInMillis() : maxDate; }
    public void setMaxDate(long maxDate) { this.maxDate = maxDate; set(year, month, day); }
    public void setFirstDayOfWeek(int firstDayOfWeek) {
        if (firstDayOfWeek < Calendar.SUNDAY || firstDayOfWeek > Calendar.SATURDAY) throw new IllegalArgumentException("firstDayOfWeek must be between 1 and 7");
        this.firstDayOfWeek = firstDayOfWeek;
    }
    public int getFirstDayOfWeek() { return firstDayOfWeek; }
    @Deprecated public boolean getCalendarViewShown() { return calendarShown; }
    @Deprecated public void setCalendarViewShown(boolean shown) { calendarShown = shown; }
    @Deprecated public boolean getSpinnersShown() { return spinnersShown; }
    @Deprecated public void setSpinnersShown(boolean shown) { spinnersShown = shown; }
    @Override public CharSequence getAccessibilityClassName() { return DatePicker.class.getName(); }
}
