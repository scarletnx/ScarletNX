// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware.display;

import android.os.Handler;
import android.view.Display;
import java.util.ArrayList;

/**
 * The displays: the default display of the window, and the virtual displays the app makes, which show nowhere (what
 * they would show goes to their surface, which nothing composes). Listeners hear of virtual displays added and
 * removed.
 */
public final class DisplayManager {
    public static final String DISPLAY_CATEGORY_PRESENTATION = "android.hardware.display.category.PRESENTATION";
    public static final int VIRTUAL_DISPLAY_FLAG_PUBLIC = 1, VIRTUAL_DISPLAY_FLAG_PRESENTATION = 2,
        VIRTUAL_DISPLAY_FLAG_SECURE = 4, VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY = 8, VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR = 16;

    public interface DisplayListener {
        void onDisplayAdded(int displayId);
        void onDisplayRemoved(int displayId);
        void onDisplayChanged(int displayId);
    }

    /* the process's virtual displays and listeners, which every DisplayManager shares, as the system's are one */
    private static final ArrayList<Display> virtuals = new ArrayList<>();
    private static final ArrayList<Object[]> listeners = new ArrayList<>(); /* {listener, handler} */
    private static int nextId = 2;

    public DisplayManager() {}

    public Display getDisplay(int displayId) {
        if (displayId == Display.DEFAULT_DISPLAY) return Display.DEFAULT;
        synchronized (virtuals) {
            for (Display d : virtuals) if (d.getDisplayId() == displayId) return d;
        }
        return null;
    }
    public Display[] getDisplays() {
        ArrayList<Display> all = new ArrayList<>();
        all.add(Display.DEFAULT);
        synchronized (virtuals) { all.addAll(virtuals); }
        return all.toArray(new Display[0]);
    }
    /** the presentation displays: the virtual ones that are */
    public Display[] getDisplays(String category) {
        if (category == null) return getDisplays();
        if (!DISPLAY_CATEGORY_PRESENTATION.equals(category)) return new Display[0];
        ArrayList<Display> out = new ArrayList<>();
        synchronized (virtuals) {
            for (Display d : virtuals) if ((d.getFlags() & Display.FLAG_PRESENTATION) != 0) out.add(d);
        }
        return out.toArray(new Display[0]);
    }
    public void registerDisplayListener(DisplayListener listener, Handler handler) {
        if (listener == null) throw new IllegalArgumentException("listener must not be null");
        synchronized (listeners) {
            for (Object[] l : listeners) if (l[0] == listener) return;
            listeners.add(new Object[] {listener, handler});
        }
    }
    public void unregisterDisplayListener(DisplayListener listener) {
        synchronized (listeners) { listeners.removeIf(l -> l[0] == listener); }
    }

    public VirtualDisplay createVirtualDisplay(String name, int width, int height, int densityDpi, android.view.Surface surface, int flags) {
        return createVirtualDisplay(name, width, height, densityDpi, surface, flags, null, null);
    }
    public VirtualDisplay createVirtualDisplay(String name, int width, int height, int densityDpi, android.view.Surface surface, int flags,
                                               VirtualDisplay.Callback callback, Handler handler) {
        if (name == null || width <= 0 || height <= 0 || densityDpi <= 0) throw new IllegalArgumentException("invalid virtual display");
        // a presentation display is public (not private) when the app asks for one, as Android's flags make it
        int displayFlags = (flags & VIRTUAL_DISPLAY_FLAG_PRESENTATION) != 0 ? Display.FLAG_PRESENTATION : 0;
        if ((flags & VIRTUAL_DISPLAY_FLAG_PUBLIC) == 0 && (flags & VIRTUAL_DISPLAY_FLAG_PRESENTATION) == 0) displayFlags |= Display.FLAG_PRIVATE;
        if ((flags & VIRTUAL_DISPLAY_FLAG_SECURE) != 0) displayFlags |= Display.FLAG_SECURE;
        Display d;
        synchronized (virtuals) {
            d = Display.virtual(nextId++, name, displayFlags, width, height, densityDpi);
            virtuals.add(d);
        }
        notify(d.getDisplayId(), 0);
        return new VirtualDisplay(this, d, surface);
    }
    void release(Display d) {
        boolean removed;
        synchronized (virtuals) { removed = virtuals.remove(d); }
        d.invalidate();
        if (removed) notify(d.getDisplayId(), 1);
    }
    /* a listener's callback on its handler (the main thread's without one): added 0, removed 1, changed 2 */
    private static void notify(int id, int what) {
        Object[][] all;
        synchronized (listeners) { all = listeners.toArray(new Object[0][]); }
        for (Object[] l : all) {
            DisplayListener listener = (DisplayListener) l[0];
            Handler h = l[1] instanceof Handler x ? x : new Handler(android.os.Looper.getMainLooper());
            h.post(() -> {
                if (what == 0) listener.onDisplayAdded(id);
                else if (what == 1) listener.onDisplayRemoved(id);
                else listener.onDisplayChanged(id);
            });
        }
    }
}
