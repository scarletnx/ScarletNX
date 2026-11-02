// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.accounts;

/** A user account: there are none. */
public class Account {
    public final String name, type;
    public Account(String name, String type) { this.name = name; this.type = type; }
}
