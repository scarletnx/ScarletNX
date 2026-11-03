// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.graphics.Bitmap;
import android.graphics.drawable.Drawable;
import java.io.IOException;
import java.io.InputStream;

/** The wallpaper service: a device without wallpaper support, so nothing is read or set. */
public class WallpaperManager {
    public static final int FLAG_SYSTEM = 1, FLAG_LOCK = 2;
    public static final String ACTION_CHANGE_LIVE_WALLPAPER = "android.service.wallpaper.CHANGE_LIVE_WALLPAPER",
        ACTION_LIVE_WALLPAPER_CHOOSER = "android.service.wallpaper.LIVE_WALLPAPER_CHOOSER",
        EXTRA_LIVE_WALLPAPER_COMPONENT = "android.service.wallpaper.extra.LIVE_WALLPAPER_COMPONENT";
    private static final WallpaperManager INSTANCE = new WallpaperManager();

    private WallpaperManager() {}
    public static WallpaperManager getInstance(android.content.Context context) { return INSTANCE; }
    public boolean isWallpaperSupported() { return false; }
    public boolean isSetWallpaperAllowed() { return false; }
    public Drawable getDrawable() { return null; }
    public Drawable getDrawable(int which) { return null; }
    public Drawable peekDrawable() { return null; }
    public Drawable getFastDrawable() { return null; }
    public Drawable peekFastDrawable() { return null; }
    public Drawable getBuiltInDrawable() { return null; }
    public int getWallpaperId(int which) { return -1; }
    /** Zero or less: the caller uses the display's size. */
    public int getDesiredMinimumWidth() { return 0; }
    public int getDesiredMinimumHeight() { return 0; }
    public void suggestDesiredDimensions(int minimumWidth, int minimumHeight) {}
    public void setBitmap(Bitmap bitmap) throws IOException {}
    public int setBitmap(Bitmap fullImage, android.graphics.Rect visibleCropHint, boolean allowBackup, int which) throws IOException { return 0; }
    public void setResource(int resid) throws IOException {}
    public int setResource(int resid, int which) throws IOException { return 0; }
    public void setStream(InputStream bitmapData) throws IOException {}
    public void clear() throws IOException {}
    public void clear(int which) throws IOException {}
}
