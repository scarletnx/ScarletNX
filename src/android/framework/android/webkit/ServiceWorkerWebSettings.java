// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** The service workers' settings, kept */
public abstract class ServiceWorkerWebSettings {
    public abstract void setCacheMode(int mode);
    public abstract int getCacheMode();
    public abstract void setAllowContentAccess(boolean allow);
    public abstract boolean getAllowContentAccess();
    public abstract void setAllowFileAccess(boolean allow);
    public abstract boolean getAllowFileAccess();
    public abstract void setBlockNetworkLoads(boolean flag);
    public abstract boolean getBlockNetworkLoads();
    static final class Values extends ServiceWorkerWebSettings {
        private int cacheMode = WebSettings.LOAD_DEFAULT;
        private boolean content = true, file, block;
        @Override public void setCacheMode(int mode) { cacheMode = mode; }
        @Override public int getCacheMode() { return cacheMode; }
        @Override public void setAllowContentAccess(boolean allow) { content = allow; }
        @Override public boolean getAllowContentAccess() { return content; }
        @Override public void setAllowFileAccess(boolean allow) { file = allow; }
        @Override public boolean getAllowFileAccess() { return file; }
        @Override public void setBlockNetworkLoads(boolean flag) { block = flag; }
        @Override public boolean getBlockNetworkLoads() { return block; }
    }
}
