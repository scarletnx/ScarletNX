// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** The pages' web storage: none is kept, as no page runs */
public class WebStorage {
    private static final WebStorage INSTANCE = new WebStorage();
    public interface QuotaUpdater { void updateQuota(long newQuota); }
    public static class Origin {
        public String getOrigin() { return null; }
        public long getQuota() { return 0; }
        public long getUsage() { return 0; }
    }
    public static WebStorage getInstance() { return INSTANCE; }
    public void getOrigins(ValueCallback<java.util.Map> callback) { if (callback != null) callback.onReceiveValue(new java.util.HashMap<>()); }
    public void getUsageForOrigin(String origin, ValueCallback<Long> callback) { if (callback != null) callback.onReceiveValue(0L); }
    public void getQuotaForOrigin(String origin, ValueCallback<Long> callback) { if (callback != null) callback.onReceiveValue(0L); }
    public void deleteOrigin(String origin) {}
    public void deleteAllData() {}
}
