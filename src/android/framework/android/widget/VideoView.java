// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.media.MediaPlayer;
import android.net.Uri;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.SurfaceView;
import java.util.Map;

/**
 * A view that plays a video. The runtime has no media player for apps yet, so each video that a VideoView opens fails
 * as a format that the device cannot play: the error listener gets MEDIA_ERROR_UNKNOWN with MEDIA_ERROR_UNSUPPORTED,
 * and when no listener takes the error, the completion listener is called, as on Android.
 */
public class VideoView extends SurfaceView implements MediaController.MediaPlayerControl {
    private static final int MEDIA_ERROR_UNKNOWN = 1, MEDIA_ERROR_UNSUPPORTED = -1010;
    private Uri uri;
    private MediaController controller;
    private MediaPlayer.OnCompletionListener onCompletion;
    private MediaPlayer.OnErrorListener onError;

    public VideoView(Context context) { this(context, null); }
    public VideoView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public VideoView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public VideoView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    public int resolveAdjustedSize(int desiredSize, int measureSpec) { return getDefaultSize(desiredSize, measureSpec); }
    public void setVideoPath(String path) { setVideoURI(Uri.parse(path)); }
    public void setVideoURI(Uri uri) { setVideoURI(uri, null); }
    public void setVideoURI(Uri uri, Map<String, String> headers) {
        this.uri = uri;
        if (uri != null) post(this::failOpen);
    }
    private void failOpen() {
        if (uri == null) return;
        android.util.Log.w("VideoView", "Unable to play video: " + uri + ": no media player");
        if (onError != null && onError.onError(null, MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_UNSUPPORTED)) return;
        if (onCompletion != null) onCompletion.onCompletion(null);
    }
    public void setAudioFocusRequest(int focusGain) {}
    public void stopPlayback() { uri = null; }
    public void setMediaController(MediaController controller) {
        if (this.controller != null) this.controller.hide();
        this.controller = controller;
        if (controller != null) { controller.setMediaPlayer(this); controller.setAnchorView(this); }
    }
    public void setOnPreparedListener(MediaPlayer.OnPreparedListener l) {} // never prepared: no media player
    public void setOnCompletionListener(MediaPlayer.OnCompletionListener l) { onCompletion = l; }
    public void setOnErrorListener(MediaPlayer.OnErrorListener l) { onError = l; }
    public void setOnInfoListener(MediaPlayer.OnInfoListener l) {}
    @Override public boolean onTouchEvent(MotionEvent ev) {
        if (ev.getActionMasked() == MotionEvent.ACTION_DOWN && controller != null) { if (controller.isShowing()) controller.hide(); else controller.show(); }
        return super.onTouchEvent(ev);
    }
    @Override public void start() {}
    @Override public void pause() {}
    public void suspend() {}
    public void resume() { if (uri != null) post(this::failOpen); }
    @Override public int getDuration() { return -1; }
    @Override public int getCurrentPosition() { return 0; }
    @Override public void seekTo(int msec) {}
    @Override public boolean isPlaying() { return false; }
    @Override public int getBufferPercentage() { return 0; }
    @Override public boolean canPause() { return false; }
    @Override public boolean canSeekBackward() { return false; }
    @Override public boolean canSeekForward() { return false; }
    @Override public int getAudioSessionId() { return 0; }
    @Override public CharSequence getAccessibilityClassName() { return VideoView.class.getName(); }
}
