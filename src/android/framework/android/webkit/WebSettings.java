// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** A WebView's settings: each one kept, and read back, though the view renders nothing (WebView) */
public abstract class WebSettings {
    public enum LayoutAlgorithm { NORMAL, SINGLE_COLUMN, NARROW_COLUMNS, TEXT_AUTOSIZING }
    public enum PluginState { ON, ON_DEMAND, OFF }
    public enum RenderPriority { NORMAL, HIGH, LOW }
    public enum ZoomDensity { FAR, MEDIUM, CLOSE }
    public static final int LOAD_DEFAULT = -1, LOAD_NORMAL = 0, LOAD_CACHE_ELSE_NETWORK = 1, LOAD_NO_CACHE = 2,
        LOAD_CACHE_ONLY = 3, MIXED_CONTENT_ALWAYS_ALLOW = 0, MIXED_CONTENT_NEVER_ALLOW = 1,
        MIXED_CONTENT_COMPATIBILITY_MODE = 2;

    public static String getDefaultUserAgent(android.content.Context context) {
        return "Mozilla/5.0 (Linux; Android 16; ScarletNX) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Mobile Safari/537.36";
    }

    /** the settings of a WebView */
    static final class Values extends WebSettings {
        private final java.util.HashMap<String, Object> values = new java.util.HashMap<>();
        private String userAgent = getDefaultUserAgent(null);
        private <T> T get(String key, T fallback) {
            @SuppressWarnings("unchecked") T v = (T) values.get(key);
            return v != null ? v : fallback;
        }
        @Override public synchronized void setJavaScriptEnabled(boolean flag) { values.put("js", flag); }
        @Override public synchronized boolean getJavaScriptEnabled() { return get("js", false); }
        @Override public synchronized void setDomStorageEnabled(boolean flag) { values.put("dom", flag); }
        @Override public synchronized boolean getDomStorageEnabled() { return get("dom", false); }
        @Override public synchronized void setUserAgentString(String ua) { userAgent = ua != null ? ua : getDefaultUserAgent(null); }
        @Override public synchronized String getUserAgentString() { return userAgent; }
        @Override public synchronized void setCacheMode(int mode) { values.put("cache", mode); }
        @Override public synchronized int getCacheMode() { return get("cache", LOAD_DEFAULT); }
        @Override public synchronized void setTextZoom(int textZoom) { values.put("zoom", textZoom); }
        @Override public synchronized int getTextZoom() { return get("zoom", 100); }
        @Override public synchronized void setMixedContentMode(int mode) { values.put("mixed", mode); }
        @Override public synchronized int getMixedContentMode() { return get("mixed", MIXED_CONTENT_NEVER_ALLOW); }
        @Override public synchronized void setLayoutAlgorithm(LayoutAlgorithm l) { values.put("layout", l); }
        @Override public synchronized LayoutAlgorithm getLayoutAlgorithm() { return get("layout", LayoutAlgorithm.NARROW_COLUMNS); }
        @Override public synchronized void setPluginState(PluginState state) { values.put("plugins", state); }
        @Override public synchronized void setFlag(String name, boolean flag) { values.put(name, flag); }
        @Override public synchronized void setValue(String name, int value) { values.put(name, value); }
    }

    public abstract void setJavaScriptEnabled(boolean flag);
    public abstract boolean getJavaScriptEnabled();
    public abstract void setDomStorageEnabled(boolean flag);
    public abstract boolean getDomStorageEnabled();
    public abstract void setUserAgentString(String ua);
    public abstract String getUserAgentString();
    public abstract void setCacheMode(int mode);
    public abstract int getCacheMode();
    public abstract void setTextZoom(int textZoom);
    public abstract int getTextZoom();
    public abstract void setMixedContentMode(int mode);
    public abstract int getMixedContentMode();
    public abstract void setLayoutAlgorithm(LayoutAlgorithm l);
    public abstract LayoutAlgorithm getLayoutAlgorithm();
    @Deprecated public abstract void setPluginState(PluginState state);
    abstract void setFlag(String name, boolean flag);
    abstract void setValue(String name, int value);

    /* the boolean and number settings that only change how a page renders */
    public void setSupportZoom(boolean support) { setFlag("supportZoom", support); }
    public void setBuiltInZoomControls(boolean enabled) { setFlag("builtInZoom", enabled); }
    public void setDisplayZoomControls(boolean enabled) { setFlag("displayZoom", enabled); }
    public void setAllowFileAccess(boolean allow) { setFlag("fileAccess", allow); }
    public void setAllowContentAccess(boolean allow) { setFlag("contentAccess", allow); }
    public void setLoadWithOverviewMode(boolean overview) { setFlag("overview", overview); }
    public void setUseWideViewPort(boolean use) { setFlag("wideViewPort", use); }
    public void setSupportMultipleWindows(boolean support) { setFlag("multipleWindows", support); }
    public void setLoadsImagesAutomatically(boolean flag) { setFlag("images", flag); }
    public void setBlockNetworkImage(boolean flag) { setFlag("blockImages", flag); }
    public void setBlockNetworkLoads(boolean flag) { setFlag("blockLoads", flag); }
    public void setDatabaseEnabled(boolean flag) { setFlag("database", flag); }
    @Deprecated public void setAppCacheEnabled(boolean flag) { setFlag("appCache", flag); }
    @Deprecated public void setAppCachePath(String path) {}
    @Deprecated public void setSavePassword(boolean save) {}
    @Deprecated public void setSaveFormData(boolean save) {}
    @Deprecated public void setGeolocationEnabled(boolean flag) { setFlag("geolocation", flag); }
    @Deprecated public void setAllowFileAccessFromFileURLs(boolean flag) { setFlag("fileFromFile", flag); }
    @Deprecated public void setAllowUniversalAccessFromFileURLs(boolean flag) { setFlag("universal", flag); }
    public void setJavaScriptCanOpenWindowsAutomatically(boolean flag) { setFlag("jsWindows", flag); }
    public void setMediaPlaybackRequiresUserGesture(boolean require) { setFlag("mediaGesture", require); }
    public void setNeedInitialFocus(boolean flag) { setFlag("initialFocus", flag); }
    public void setDefaultTextEncodingName(String encoding) {}
    public void setDefaultFontSize(int size) { setValue("fontSize", size); }
    public void setMinimumFontSize(int size) { setValue("minFontSize", size); }
    @Deprecated public void setRenderPriority(RenderPriority priority) {}
    @Deprecated public void setDefaultZoom(ZoomDensity zoom) {}
    public void setOffscreenPreRaster(boolean enabled) { setFlag("preRaster", enabled); }
    public void setSafeBrowsingEnabled(boolean enabled) { setFlag("safeBrowsing", enabled); }
    public void setForceDark(int forceDark) { setValue("forceDark", forceDark); }
    public void setAlgorithmicDarkeningAllowed(boolean allow) { setFlag("darkening", allow); }
}
