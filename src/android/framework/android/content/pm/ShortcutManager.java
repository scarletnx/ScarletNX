// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

import android.content.Intent;
import android.content.IntentSender;
import java.util.ArrayList;
import java.util.List;

/**
 * The app's launcher shortcuts, kept in the process. The device has no launcher that shows or pins them, so a pin
 * request is not supported, as on a device whose launcher can't pin.
 */
public class ShortcutManager {
    public static final int FLAG_MATCH_MANIFEST = 1, FLAG_MATCH_DYNAMIC = 2, FLAG_MATCH_PINNED = 4, FLAG_MATCH_CACHED = 8;
    private static final int MAX = 15;
    private final ArrayList<ShortcutInfo> dynamic = new ArrayList<>();

    public ShortcutManager() {}

    public synchronized boolean setDynamicShortcuts(List<ShortcutInfo> shortcuts) {
        if (shortcuts.size() > MAX) throw new IllegalArgumentException("Max number of dynamic shortcuts exceeded");
        dynamic.clear();
        dynamic.addAll(shortcuts);
        return true;
    }
    public synchronized List<ShortcutInfo> getDynamicShortcuts() { return new ArrayList<>(dynamic); }
    public List<ShortcutInfo> getManifestShortcuts() { return new ArrayList<>(); }
    public List<ShortcutInfo> getPinnedShortcuts() { return new ArrayList<>(); }
    public synchronized List<ShortcutInfo> getShortcuts(int matchFlags) {
        return (matchFlags & FLAG_MATCH_DYNAMIC) != 0 ? new ArrayList<>(dynamic) : new ArrayList<>();
    }
    public synchronized boolean addDynamicShortcuts(List<ShortcutInfo> shortcuts) {
        for (ShortcutInfo s : shortcuts) {
            dynamic.removeIf(d -> d.id.equals(s.id));
            dynamic.add(s);
        }
        return true;
    }
    public synchronized void pushDynamicShortcut(ShortcutInfo shortcut) {
        dynamic.removeIf(d -> d.id.equals(shortcut.id));
        dynamic.add(shortcut);
        while (dynamic.size() > MAX) dynamic.remove(0);
    }
    public synchronized void removeDynamicShortcuts(List<String> ids) { dynamic.removeIf(d -> ids.contains(d.id)); }
    public synchronized void removeAllDynamicShortcuts() { dynamic.clear(); }
    public synchronized void removeLongLivedShortcuts(List<String> ids) { removeDynamicShortcuts(ids); }
    public synchronized boolean updateShortcuts(List<ShortcutInfo> shortcuts) {
        for (ShortcutInfo s : shortcuts)
            for (int i = 0; i < dynamic.size(); ++i) if (dynamic.get(i).id.equals(s.id)) dynamic.set(i, s);
        return true;
    }
    public synchronized void disableShortcuts(List<String> ids) { for (ShortcutInfo d : dynamic) if (ids.contains(d.id)) d.enabled = false; }
    public void disableShortcuts(List<String> ids, CharSequence disabledMessage) { disableShortcuts(ids); }
    public synchronized void enableShortcuts(List<String> ids) { for (ShortcutInfo d : dynamic) if (ids.contains(d.id)) d.enabled = true; }
    public int getMaxShortcutCountPerActivity() { return MAX; }
    public boolean isRateLimitingActive() { return false; }
    public int getIconMaxWidth() { return 192; }
    public int getIconMaxHeight() { return 192; }
    public void reportShortcutUsed(String shortcutId) {}
    public boolean isRequestPinShortcutSupported() { return false; }
    public boolean requestPinShortcut(ShortcutInfo shortcut, IntentSender resultIntent) { return false; }
    public Intent createShortcutResultIntent(ShortcutInfo shortcut) { return new Intent(); }
}
