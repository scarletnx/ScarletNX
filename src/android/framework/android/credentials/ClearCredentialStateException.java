// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

/** An error of a credential state clear. */
public class ClearCredentialStateException extends Exception {
    public static final String TYPE_UNKNOWN = "android.credentials.ClearCredentialStateException.TYPE_UNKNOWN";
    private final String type;

    public ClearCredentialStateException(String type, String message) { this(type, message, null); }
    public ClearCredentialStateException(String type, String message, Throwable cause) { super(message, cause); this.type = type; }
    public ClearCredentialStateException(String type, Throwable cause) { this(type, null, cause); }
    public ClearCredentialStateException(String type) { this(type, null, null); }
    public String getType() { return type; }
    @Override public String getMessage() { return super.getMessage(); }
}
