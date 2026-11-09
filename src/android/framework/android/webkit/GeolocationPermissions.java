// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** The pages' location permissions by origin, which nothing asks for, as no page runs */
public class GeolocationPermissions {
    private static final GeolocationPermissions INSTANCE = new GeolocationPermissions();
    public interface Callback { void invoke(String origin, boolean allow, boolean retain); }
    public static GeolocationPermissions getInstance() { return INSTANCE; }
    public void getOrigins(ValueCallback<java.util.Set<String>> callback) { if (callback != null) callback.onReceiveValue(new java.util.HashSet<>()); }
    public void getAllowed(String origin, ValueCallback<Boolean> callback) { if (callback != null) callback.onReceiveValue(false); }
    public void clear(String origin) {}
    public void allow(String origin) {}
    public void clearAll() {}
}
