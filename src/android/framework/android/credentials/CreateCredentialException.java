// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

/** An error of a credential creation. */
public class CreateCredentialException extends Exception {
    public static final String TYPE_UNKNOWN = "android.credentials.CreateCredentialException.TYPE_UNKNOWN";
    public static final String TYPE_NO_CREATE_OPTIONS = "android.credentials.CreateCredentialException.TYPE_NO_CREATE_OPTIONS";
    public static final String TYPE_USER_CANCELED = "android.credentials.CreateCredentialException.TYPE_USER_CANCELED";
    public static final String TYPE_INTERRUPTED = "android.credentials.CreateCredentialException.TYPE_INTERRUPTED";
    private final String type;

    public CreateCredentialException(String type, String message) { this(type, message, null); }
    public CreateCredentialException(String type, String message, Throwable cause) { super(message, cause); this.type = type; }
    public CreateCredentialException(String type, Throwable cause) { this(type, null, cause); }
    public CreateCredentialException(String type) { this(type, null, null); }
    public String getType() { return type; }
    @Override public String getMessage() { return super.getMessage(); }
}
