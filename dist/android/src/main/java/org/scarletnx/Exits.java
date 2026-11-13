// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import java.util.List;

/** What Android says of how a process of the app ended, for a start's mark (src/common/utilities/run_mark.h): a start
    whose mark no process holds did not end with an exit, and ApplicationExitInfo (Android 11) keeps why */
final class Exits {
    /** ApplicationExitInfo's REASON_*, by their number: Android 11's 0 to 13, Android 13's 14, Android 14's 15, 16 */
    private static final String[] REASONS = {"unknown", "exit self", "signaled", "low memory", "crash", "crash native",
                                             "ANR", "initialization failure", "permission change",
                                             "excessive resource usage", "user requested", "user stopped",
                                             "dependency died", "other", "freezer", "package state change",
                                             "package updated"};

    private Exits() {}

    /** a REASON_*'s name, its number for one past those Android 14 has */
    static String reasonName(int reason) {
        return reason >= 0 && reason < REASONS.length ? REASONS[reason] : "reason " + reason;
    }

    /** RunningAppProcessInfo's IMPORTANCE_* a process had as it ended: the number, with its name when it has one */
    static String importance(int importance) {
        switch (importance) {
        case 100: return "100 foreground";
        case 125: return "125 foreground service";
        case 200: return "200 visible";
        case 230: return "230 perceptible";
        case 300: return "300 service";
        case 325: return "325 cannot save state";
        case 400: return "400 cached";
        case 1000: return "1000 gone";
        default: return String.valueOf(importance);
        }
    }

    /** the end of process pid of the app, which started at since (ms since 1970, 0 when unknown) */
    static String of(Context context, int pid, long since) {
        ActivityManager manager = (ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
        return of(manager, context.getPackageName(), Build.VERSION.SDK_INT, pid, since);
    }

    /** the same on a system of API level sdk: the first end of pid since it started (the newest when since is 0), what
        Android says of it, or that it says none. The system gives the ends of that pid alone, the ones before it
        started too, as pids come round again */
    static String of(ActivityManager manager, String app, int sdk, int pid, long since) {
        if (sdk < 30) return none(sdk);
        try {
            ApplicationExitInfo end = end(manager, app, pid, since);
            if (end == null) return kept(pid, since);
            String description = end.getDescription();
            return "Android says " + reasonName(end.getReason()) + " (" + end.getProcessName() + ", status " +
                end.getStatus() + ", importance " + importance(end.getImportance()) + ", " + (end.getRss() >> 10) +
                " MiB resident)" + (description != null && !description.isEmpty() ? ": " + description : "");
        } catch (RuntimeException e) {
            return "Android's reason cannot be read: " + e;
        }
    }

    /** the same end as fields, a "key=value" line each, for the launcher's Last Run (src/gui/last_run.h): reason (the
        REASON_* number), said (its name), process, status, importance, pss and rss (KiB), description; or a line
        none= saying why there is none. A value keeps no line break */
    static String fields(ActivityManager manager, String app, int sdk, int pid, long since) {
        if (sdk < 30) return "none=" + none(sdk) + "\n";
        try {
            ApplicationExitInfo end = end(manager, app, pid, since);
            if (end == null) return "none=" + kept(pid, since) + "\n";
            String description = end.getDescription();
            return "reason=" + end.getReason() + "\nsaid=" + reasonName(end.getReason()) + "\nprocess=" +
                line(end.getProcessName()) + "\nstatus=" + end.getStatus() + "\nimportance=" +
                importance(end.getImportance()) + "\npss=" + end.getPss() + "\nrss=" + end.getRss() + "\ndescription=" +
                line(description) + "\n";
        } catch (RuntimeException e) {
            return "none=Android's reason cannot be read: " + line(e.toString()) + "\n";
        }
    }

    private static String none(int sdk) {
        return "Android (API " + sdk + ") does not say why a process ended, as Android 11 (API 30) does";
    }

    private static String kept(int pid, long since) {
        return "Android keeps no end of pid " + pid + (since > 0 ? " since it started" : "");
    }

    /** text on one line, "" for none */
    private static String line(String text) {
        return text == null ? "" : text.replace('\n', ' ').replace('\r', ' ');
    }

    /** the first end of pid at or after since (ms since 1970), the newest when since is 0; null for none */
    private static ApplicationExitInfo end(ActivityManager manager, String app, int pid, long since) {
        List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(app, pid, 0);
        ApplicationExitInfo end = null;
        for (ApplicationExitInfo exit : exits) {
            if (exit.getTimestamp() < since) continue;
            boolean first = since > 0 ? end == null || exit.getTimestamp() < end.getTimestamp()
                                      : end == null || exit.getTimestamp() > end.getTimestamp();
            if (first) end = exit;
        }
        return end;
    }
}
