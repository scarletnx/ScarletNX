// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Toast;
import java.util.List;
import java.util.Locale;

/** org.scarletnx.action.PLAY: a game played by its title ID, from adb (docs/hosts/android-app.md) or a shortcut. It
    shows nothing: the menu finds the game and plays it as Games' Play does, or says why not (src/gui/main.c's
    play_asked); this only hands it the ID */
public class PlayActivity extends Activity {
    /** the title ID names the game: the name the console gives it and the menu's folders take, which stays the same
        when the game is recompiled, moved or renamed */
    public static final String ACTION = "org.scarletnx.action.PLAY", TITLE_ID = "title_id";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        play(this, getIntent().getStringExtra(TITLE_ID));
        finish();
    }

    /** whether a game's process runs: GameActivity's (android:process=":game"), over the menu in its task */
    static boolean gameRuns(Context context) {
        List<ActivityManager.RunningAppProcessInfo> processes =
            ((ActivityManager)context.getSystemService(ACTIVITY_SERVICE)).getRunningAppProcesses();
        String game = context.getPackageName() + ":game";
        if (processes != null)
            for (ActivityManager.RunningAppProcessInfo process : processes)
                if (game.equals(process.processName)) return true;
        return false;
    }

    /** id, as given, to the menu. With none, one that is no title ID, or a game running, the menu comes in front as
        its icon brings it (the game that runs over it) and a toast says why, naming what was given; none for the game
        that runs, which comes back */
    static void play(Activity from, String given) {
        String id = given != null ? given.trim().toUpperCase(Locale.ROOT) : "", refused = null;
        boolean runs = gameRuns(from);
        if (id.isEmpty())
            refused = "No title_id: PLAY names the game by its title ID";
        else if (!id.matches("[0-9A-F]{16}"))
            refused = "title_id " + given + " is not a title ID (16 hex digits)";
        else if (runs && !id.equals(MainActivity.playing()))
            refused = "A game is running: close it to play " + id;
        if (refused != null) Toast.makeText(from.getApplicationContext(), refused, Toast.LENGTH_LONG).show();
        if (refused != null || runs) {
            from.startActivity(MainActivity.fromIcon(from));
            return;
        }
        // CLEAR_TOP closes what the menu opened over itself (the folder picker, a settings screen), so the intent
        // reaches the menu itself, never a second one on top; it would close a game too, so one running only comes
        // back, above
        from.startActivity(MainActivity.fromIcon(from).putExtra(TITLE_ID, id).addFlags(
            Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP));
    }
}
