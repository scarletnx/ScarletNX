// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.appsetid;

/** An ID that the apps of one developer share. */
public class AppSetId {
    public static final int SCOPE_APP = 1, SCOPE_DEVELOPER = 2;
    private final String id;
    private final int scope;

    public AppSetId(String appSetId, int appSetIdScope) { id = appSetId; scope = appSetIdScope; }
    public String getId() { return id; }
    public int getScope() { return scope; }
    @Override public boolean equals(Object o) { return o instanceof AppSetId a && id.equals(a.id) && scope == a.scope; }
    @Override public int hashCode() { return id.hashCode() * 31 + scope; }
}
