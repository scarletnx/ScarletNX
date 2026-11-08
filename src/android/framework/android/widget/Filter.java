// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.os.Handler;
import android.os.Looper;

/** Filters the data of an adapter on a worker thread, and publishes the result on the thread that asked. */
public abstract class Filter {
    private final Object lock = new Object();
    private int generation;

    public Filter() {}

    public static class FilterResults {
        public Object values;
        public int count;
        public FilterResults() {}
    }

    public interface FilterListener { void onFilterComplete(int count); }

    public final void filter(CharSequence constraint) { filter(constraint, null); }

    /** A new request replaces the one that waits: only the result of the last request is published. */
    public final void filter(CharSequence constraint, FilterListener listener) {
        Looper looper = Looper.myLooper();
        Handler handler = new Handler(looper != null ? looper : Looper.getMainLooper());
        int id;
        synchronized (lock) { id = ++generation; }
        Thread t = new Thread(() -> {
            FilterResults results;
            try { results = performFiltering(constraint); }
            catch (RuntimeException e) { android.util.Log.w("Filter", "An exception occurred during performFiltering()!", e); results = null; }
            FilterResults r = results;
            handler.post(() -> {
                synchronized (lock) { if (id != generation) return; }
                publishResults(constraint, r);
                if (listener != null) listener.onFilterComplete(r != null ? r.count : -1);
            });
        }, "Filter");
        t.setDaemon(true);
        t.start();
    }

    protected abstract FilterResults performFiltering(CharSequence constraint);
    protected abstract void publishResults(CharSequence constraint, FilterResults results);
    public CharSequence convertResultToString(Object resultValue) { return resultValue == null ? "" : resultValue.toString(); }
}
