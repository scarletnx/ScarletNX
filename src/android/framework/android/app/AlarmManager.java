// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** Alarms: run as delayed work of the process, which lives as long as the app is open. */
public class AlarmManager {
    public static final int RTC_WAKEUP = 0, RTC = 1, ELAPSED_REALTIME_WAKEUP = 2, ELAPSED_REALTIME = 3;
    public static final long INTERVAL_FIFTEEN_MINUTES = 900000, INTERVAL_HALF_HOUR = 1800000, INTERVAL_HOUR = 3600000,
        INTERVAL_DAY = 86400000;
    private final java.util.HashMap<PendingIntent, Runnable> alarms = new java.util.HashMap<>();
    private AlarmClockInfo nextClock;
    private PendingIntent nextClockOperation;
    private final android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());

    private long delay(int type, long triggerAtMillis) {
        long now = type == RTC || type == RTC_WAKEUP ? System.currentTimeMillis() : android.os.SystemClock.elapsedRealtime();
        return Math.max(0, triggerAtMillis - now);
    }
    public void set(int type, long triggerAtMillis, PendingIntent operation) {
        cancel(operation);
        Runnable r = () -> { synchronized (alarms) { alarms.remove(operation); } operation.sendQuietly(); };
        synchronized (alarms) { alarms.put(operation, r); }
        handler.postDelayed(r, delay(type, triggerAtMillis));
    }
    public void setExact(int type, long triggerAtMillis, PendingIntent operation) { set(type, triggerAtMillis, operation); }
    public void setExactAndAllowWhileIdle(int type, long triggerAtMillis, PendingIntent operation) { set(type, triggerAtMillis, operation); }
    public void setAndAllowWhileIdle(int type, long triggerAtMillis, PendingIntent operation) { set(type, triggerAtMillis, operation); }
    public void setWindow(int type, long windowStartMillis, long windowLengthMillis, PendingIntent operation) { set(type, windowStartMillis, operation); }
    public void setRepeating(int type, long triggerAtMillis, long intervalMillis, PendingIntent operation) { set(type, triggerAtMillis, operation); }
    public void setInexactRepeating(int type, long triggerAtMillis, long intervalMillis, PendingIntent operation) { set(type, triggerAtMillis, operation); }
    public void cancel(PendingIntent operation) {
        Runnable r;
        synchronized (alarms) { r = alarms.remove(operation); }
        if (r != null) handler.removeCallbacks(r);
    }
    public boolean canScheduleExactAlarms() { return true; }
    /** An alarm clock: an RTC_WAKEUP alarm, which getNextAlarmClock() reports until it goes off or is canceled. */
    public void setAlarmClock(AlarmClockInfo info, PendingIntent operation) {
        set(RTC_WAKEUP, info.getTriggerTime(), operation);
        synchronized (alarms) { nextClock = info; nextClockOperation = operation; }
    }
    public AlarmClockInfo getNextAlarmClock() { synchronized (alarms) { return alarms.containsKey(nextClockOperation) ? nextClock : null; } }

    public static final class AlarmClockInfo implements android.os.Parcelable {
        private final long triggerTime;
        private final PendingIntent showIntent;
        public AlarmClockInfo(long triggerTime, PendingIntent showIntent) { this.triggerTime = triggerTime; this.showIntent = showIntent; }
        public long getTriggerTime() { return triggerTime; }
        public PendingIntent getShowIntent() { return showIntent; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
    }
}
