// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

/** An error of a credential retrieval. */
public class GetCredentialException extends Exception {
    public static final String TYPE_UNKNOWN = "android.credentials.GetCredentialException.TYPE_UNKNOWN";
    public static final String TYPE_NO_CREDENTIAL = "android.credentials.GetCredentialException.TYPE_NO_CREDENTIAL";
    public static final String TYPE_USER_CANCELED = "android.credentials.GetCredentialException.TYPE_USER_CANCELED";
    public static final String TYPE_INTERRUPTED = "android.credentials.GetCredentialException.TYPE_INTERRUPTED";
    private final String type;

    public GetCredentialException(String type, String message) { this(type, message, null); }
    public GetCredentialException(String type, String message, Throwable cause) { super(message, cause); this.type = type; }
    public GetCredentialException(String type, Throwable cause) { this(type, null, cause); }
    public GetCredentialException(String type) { this(type, null, null); }
    public String getType() { return type; }
    @Override public String getMessage() { return super.getMessage(); }
}
