// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

/** How many datasets of a type a service has */
public final class SavedDatasetsInfo {
    public static final String TYPE_OTHER = "other", TYPE_PASSWORDS = "passwords";
    private final String type;
    private final int count;
    public SavedDatasetsInfo(String type, int count) {
        this.type = type;
        this.count = count;
    }
    public String getType() { return type; }
    public int getCount() { return count; }
}
