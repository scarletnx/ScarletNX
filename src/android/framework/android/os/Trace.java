// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** Systrace sections: nothing traces, so they do nothing. */
public final class Trace {
    private Trace() {}

    public static boolean isEnabled() { return false; }
    public static void beginSection(String name) {}
    public static void endSection() {}
    public static void beginAsyncSection(String name, int cookie) {}
    public static void endAsyncSection(String name, int cookie) {}
    public static void setCounter(String name, long value) {}
    public static boolean isTagEnabled(long tag) { return false; }
    public static void traceBegin(long tag, String name) {}
    public static void traceEnd(long tag) {}
    public static void asyncTraceBegin(long tag, String name, int cookie) {}
    public static void asyncTraceEnd(long tag, String name, int cookie) {}
    public static void traceCounter(long tag, String name, int value) {}
}
