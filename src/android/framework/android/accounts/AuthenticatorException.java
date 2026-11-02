// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.accounts;

/** An authenticator's failure. */
public class AuthenticatorException extends android.util.AndroidException {
    public AuthenticatorException() {}
    public AuthenticatorException(String message) { super(message); }
    public AuthenticatorException(String message, Throwable cause) { super(message); initCause(cause); }
    public AuthenticatorException(Exception cause) { super(cause != null ? cause.toString() : null); initCause(cause); }
}
