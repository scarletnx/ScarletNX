// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.ComponentName;
import android.content.Context;
import android.os.Parcel;
import android.os.Parcelable;

/**
 * A report of an app's crash, hang or battery use, for the store it came from. No store takes reports here:
 * getErrorReportReceiver, the hidden call that apps ask which receiver takes them, gives none.
 */
public class ApplicationErrorReport implements Parcelable {
    public static final int TYPE_NONE = 0, TYPE_CRASH = 1, TYPE_ANR = 2, TYPE_BATTERY = 3, TYPE_RUNNING_SERVICE = 5;

    public int type;
    public String packageName, installerPackageName, processName;
    public long time;
    public boolean systemApp;
    public CrashInfo crashInfo;

    public ApplicationErrorReport() {}

    /** The receiver of the installer of packageName that takes error reports: none here */
    public static ComponentName getErrorReportReceiver(Context context, String packageName, int appFlags) { return null; }

    public static class CrashInfo {
        public String exceptionClassName, exceptionMessage, throwFileName, throwClassName, throwMethodName, stackTrace;
        public int throwLineNumber;

        public CrashInfo() {}
        public CrashInfo(Throwable tr) {
            java.io.StringWriter sw = new java.io.StringWriter();
            tr.printStackTrace(new java.io.PrintWriter(sw));
            stackTrace = sw.toString();
            Throwable root = tr;
            while (root.getCause() != null) root = root.getCause();
            exceptionClassName = root.getClass().getName();
            exceptionMessage = root.getMessage();
            StackTraceElement[] trace = root.getStackTrace();
            if (trace.length > 0) {
                throwFileName = trace[0].getFileName();
                throwClassName = trace[0].getClassName();
                throwMethodName = trace[0].getMethodName();
                throwLineNumber = trace[0].getLineNumber();
            }
        }
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(type);
        dest.writeString(packageName);
        dest.writeString(installerPackageName);
        dest.writeString(processName);
        dest.writeLong(time);
        dest.writeInt(systemApp ? 1 : 0);
    }
    public static final Creator<ApplicationErrorReport> CREATOR = new Creator<ApplicationErrorReport>() {
        @Override public ApplicationErrorReport createFromParcel(Parcel in) {
            ApplicationErrorReport r = new ApplicationErrorReport();
            r.type = in.readInt();
            r.packageName = in.readString();
            r.installerPackageName = in.readString();
            r.processName = in.readString();
            r.time = in.readLong();
            r.systemApp = in.readInt() != 0;
            return r;
        }
        @Override public ApplicationErrorReport[] newArray(int size) { return new ApplicationErrorReport[size]; }
    };
}
