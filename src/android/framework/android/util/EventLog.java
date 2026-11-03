// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** The binary event log. No log reader is here, so events go nowhere and none is read. */
public class EventLog {
    private EventLog() {}

    public static int writeEvent(int tag, int value) { return 0; }
    public static int writeEvent(int tag, long value) { return 0; }
    public static int writeEvent(int tag, float value) { return 0; }
    public static int writeEvent(int tag, String str) { return 0; }
    public static int writeEvent(int tag, Object... list) { return 0; }
    public static String getTagName(int tag) { return null; }
    public static int getTagCode(String name) { return -1; }
}
