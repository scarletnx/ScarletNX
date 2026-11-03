// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

/**
 * The controls of a media player: play, pause and seek. On Android they show in a window over the anchor view. The
 * runtime has no such window, so the controller keeps its state and the app drives the player.
 */
public class MediaController extends FrameLayout {
    private static final int DEFAULT_TIMEOUT = 3000;
    private MediaPlayerControl player;
    private boolean showing;
    private final Runnable hide = this::hide;

    public interface MediaPlayerControl {
        void start();
        void pause();
        int getDuration();
        int getCurrentPosition();
        void seekTo(int pos);
        boolean isPlaying();
        int getBufferPercentage();
        boolean canPause();
        boolean canSeekBackward();
        boolean canSeekForward();
        int getAudioSessionId();
    }

    public MediaController(Context context, AttributeSet attrs) { super(context, attrs); }
    public MediaController(Context context, boolean useFastForward) { super(context); }
    public MediaController(Context context) { this(context, true); }

    public void setMediaPlayer(MediaPlayerControl player) { this.player = player; }
    public void setAnchorView(View view) {}
    public void show() { show(DEFAULT_TIMEOUT); }
    /** Shows the controls; they hide after the timeout, or stay until hide() when the timeout is 0. */
    public void show(int timeout) {
        showing = true;
        removeCallbacks(hide);
        if (timeout != 0) postDelayed(hide, timeout);
    }
    public boolean isShowing() { return showing; }
    public void hide() { removeCallbacks(hide); showing = false; }
    public void setPrevNextListeners(View.OnClickListener next, View.OnClickListener prev) {}
    @Override public boolean onTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: show(0); break;
            case MotionEvent.ACTION_UP: show(DEFAULT_TIMEOUT); break;
            case MotionEvent.ACTION_CANCEL: hide(); break;
            default: break;
        }
        return true;
    }
    @Override public CharSequence getAccessibilityClassName() { return MediaController.class.getName(); }
}
