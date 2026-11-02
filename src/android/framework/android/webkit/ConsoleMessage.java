// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

public class ConsoleMessage {
    public enum MessageLevel { TIP, LOG, WARNING, ERROR, DEBUG }
    private final String message, sourceId;
    private final int line;
    private final MessageLevel level;
    public ConsoleMessage(String message, String sourceId, int lineNumber, MessageLevel level) {
        this.message = message;
        this.sourceId = sourceId;
        line = lineNumber;
        this.level = level;
    }
    public MessageLevel messageLevel() { return level; }
    public String message() { return message; }
    public String sourceId() { return sourceId; }
    public int lineNumber() { return line; }
}
