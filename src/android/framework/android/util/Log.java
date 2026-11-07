// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.io.PrintWriter;
import java.io.StringWriter;

/** The app's log: "D/Tag: message" lines on the runtime's stderr. SNX_LOG_LEVEL (V, D, I, W, E) filters them. */
public final class Log {
    public static final int VERBOSE = 2;
    public static final int DEBUG = 3;
    public static final int INFO = 4;
    public static final int WARN = 5;
    public static final int ERROR = 6;
    public static final int ASSERT = 7;
    private static final String LETTERS = "??VDIWEA";
    private static final int LEVEL;
    static {
        String l = System.getProperty("snx.android.log_level");
        int level = DEBUG;
        if (l != null && l.length() == 1 && LETTERS.indexOf(l.charAt(0)) >= VERBOSE) level = LETTERS.indexOf(l.charAt(0));
        LEVEL = level;
    }

    private Log() {}

    public static boolean isLoggable(String tag, int level) { return level >= LEVEL; }

    public static int println(int priority, String tag, String msg) {
        if (priority < LEVEL) return 0;
        String line = LETTERS.charAt(Math.max(0, Math.min(priority, 7))) + "/" + tag + ": " + msg;
        System.err.println(line);
        return line.length();
    }

    private static int log(int priority, String tag, String msg, Throwable tr) {
        return println(priority, tag, tr == null ? msg : msg + '\n' + getStackTraceString(tr));
    }

    public static int v(String tag, String msg) { return log(VERBOSE, tag, msg, null); }
    public static int v(String tag, String msg, Throwable tr) { return log(VERBOSE, tag, msg, tr); }
    public static int d(String tag, String msg) { return log(DEBUG, tag, msg, null); }
    public static int d(String tag, String msg, Throwable tr) { return log(DEBUG, tag, msg, tr); }
    public static int i(String tag, String msg) { return log(INFO, tag, msg, null); }
    public static int i(String tag, String msg, Throwable tr) { return log(INFO, tag, msg, tr); }
    public static int w(String tag, String msg) { return log(WARN, tag, msg, null); }
    public static int w(String tag, String msg, Throwable tr) { return log(WARN, tag, msg, tr); }
    public static int w(String tag, Throwable tr) { return log(WARN, tag, "", tr); }
    public static int e(String tag, String msg) { return log(ERROR, tag, msg, null); }
    public static int e(String tag, String msg, Throwable tr) { return log(ERROR, tag, msg, tr); }
    public static int wtf(String tag, String msg) { return log(ASSERT, tag, msg, null); }
    public static int wtf(String tag, Throwable tr) { return log(ASSERT, tag, String.valueOf(tr), tr); }
    public static int wtf(String tag, String msg, Throwable tr) { return log(ASSERT, tag, msg, tr); }

    public static String getStackTraceString(Throwable tr) {
        if (tr == null) return "";
        StringWriter sw = new StringWriter();
        tr.printStackTrace(new PrintWriter(sw));
        return sw.toString();
    }
}
