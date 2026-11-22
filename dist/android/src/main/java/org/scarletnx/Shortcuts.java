// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.annotation.TargetApi;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Icon;
import android.text.TextUtils;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** The games on the app's icon and on the home screen: a long press on the icon lists the games played last (Android
    7.1 and later), and a game's box in the menu adds it to the home screen (Android 8 and later). Each opens the game
    through PlayActivity, by its title ID, as adb does */
final class Shortcuts {
    /** the icon's games, newest first, their IDs joined by commas: the order the oldest goes by */
    static final String ORDER = "order";

    private Shortcuts() {}

    /** the PLAY of the game whose title ID is id */
    static Intent play(Context context, String id) {
        return new Intent(PlayActivity.ACTION)
            .setClass(context, PlayActivity.class)
            .putExtra(PlayActivity.TITLE_ID, id);
    }

    /** the picture at path (an image the system decodes), else the app's own icon: for none, or a 3DS game's raw
        icon.rgba */
    @TargetApi(23)
    static Icon icon(Context context, String path) {
        Bitmap picture = path != null ? BitmapFactory.decodeFile(path) : null;
        return picture != null ? Icon.createWithBitmap(picture)
                               : Icon.createWithResource(context, R.mipmap.ic_launcher);
    }

    @TargetApi(25)
    private static ShortcutInfo shortcut(Context context, String id, String name, String icon) {
        String label = name != null && !name.isEmpty() ? name : id;
        return new ShortcutInfo.Builder(context, id)
            .setShortLabel(label)
            .setLongLabel(label)
            .setIcon(icon(context, icon))
            .setIntent(play(context, id))
            .setRank(0)
            .build();
    }

    /** a game started, on sdk (Build.VERSION.SDK_INT): first of the icon's shortcuts, the oldest past the launcher's
        own count (getMaxShortcutCountPerActivity) taken off. "" when done or on an Android without them, else what
        failed */
    @TargetApi(25)
    static String played(Context context, int sdk, String id, String name, String icon) {
        if (sdk < 25) return "";
        ShortcutManager manager = context.getSystemService(ShortcutManager.class);
        SharedPreferences kept = context.getSharedPreferences("shortcuts", Context.MODE_PRIVATE);
        List<String> listed = new ArrayList<>(), order = new ArrayList<>(), gone = new ArrayList<>();
        try {
            for (ShortcutInfo shortcut : manager.getDynamicShortcuts()) listed.add(shortcut.getId());
            // the order kept, of the shortcuts still there, then any it does not know (an older install's) as oldest
            order.add(id);
            for (String was : kept.getString(ORDER, "").split(","))
                if (listed.contains(was) && !order.contains(was)) order.add(was);
            for (String was : listed)
                if (!order.contains(was)) order.add(was);
            int count = Math.max(1, manager.getMaxShortcutCountPerActivity());
            while (order.size() > count) gone.add(order.remove(order.size() - 1));
            if (!gone.isEmpty()) manager.removeDynamicShortcuts(gone);
            // rank 0 puts it first, the others after it as they were
            if (!manager.addDynamicShortcuts(Collections.singletonList(shortcut(context, id, name, icon))))
                return "the system holds the app's shortcuts back (rate limited): " + id + " is not on its icon";
            manager.reportShortcutUsed(id);
        } catch (IllegalArgumentException | IllegalStateException e) {
            return "the shortcut of " + id + " was refused: " + e;
        }
        kept.edit().putString(ORDER, TextUtils.join(",", order)).apply();
        return "";
    }

    /** the game's shortcut asked of the home screen, on sdk, which asks the player before it adds it: "" when asked,
        else why not, naming the Android it needs and the one here */
    @TargetApi(26)
    static String pin(Context context, int sdk, String id, String name, String icon) {
        if (sdk < 26)
            return "Adding to the home screen needs Android 8 (API 26): this one is API " + sdk;
        ShortcutManager manager = context.getSystemService(ShortcutManager.class);
        if (!manager.isRequestPinShortcutSupported()) return "This home screen takes no shortcuts";
        try {
            if (!manager.requestPinShortcut(shortcut(context, id, name, icon), null))
                return "The home screen did not take the shortcut of " + id;
        } catch (IllegalArgumentException | IllegalStateException e) {
            return "The home screen refused the shortcut of " + id + ": " + e;
        }
        return "";
    }
}
