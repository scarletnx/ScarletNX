// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.MotionEvent;

/** A progress bar with a thumb that the user drags to set the progress. */
public abstract class AbsSeekBar extends ProgressBar {
    private Drawable thumb;
    private int thumbOffset, keyProgressIncrement = 1;
    private boolean splitTrack;

    public AbsSeekBar(Context context) { this(context, null); }
    public AbsSeekBar(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public AbsSeekBar(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public AbsSeekBar(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setFocusable(true);
        setClickable(true);
    }

    public void setThumb(Drawable thumb) {
        if (this.thumb != null && this.thumb != thumb) this.thumb.setCallback(null);
        this.thumb = thumb;
        if (thumb != null) {
            thumb.setCallback(this);
            thumbOffset = thumb.getIntrinsicWidth() / 2;
            if (thumb.isStateful()) thumb.setState(getDrawableState());
        }
        requestLayout();
        invalidate();
    }
    public Drawable getThumb() { return thumb; }
    public int getThumbOffset() { return thumbOffset; }
    public void setThumbOffset(int thumbOffset) { this.thumbOffset = thumbOffset; invalidate(); }
    public void setSplitTrack(boolean splitTrack) { this.splitTrack = splitTrack; }
    public boolean getSplitTrack() { return splitTrack; }
    public void setKeyProgressIncrement(int increment) { keyProgressIncrement = increment < 0 ? -increment : increment; }
    public int getKeyProgressIncrement() { return keyProgressIncrement; }
    @Override public synchronized void setMax(int max) {
        super.setMax(max);
        int range = getMax() - getMin();
        if (keyProgressIncrement == 0 || range / keyProgressIncrement > 20) setKeyProgressIncrement(Math.max(1, Math.round(range / 20f)));
    }

    @Override protected boolean verifyDrawable(Drawable who) { return who == thumb || super.verifyDrawable(who); }
    @Override protected void drawableStateChanged() {
        super.drawableStateChanged();
        if (thumb != null && thumb.isStateful() && thumb.setState(getDrawableState())) invalidateDrawable(thumb);
    }
    @Override public void jumpDrawablesToCurrentState() { super.jumpDrawablesToCurrentState(); if (thumb != null) thumb.jumpToCurrentState(); }
    @Override protected synchronized void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (thumb == null) return;
        int available = getWidth() - getPaddingLeft() - getPaddingRight() - thumb.getIntrinsicWidth() + 2 * thumbOffset;
        int x = getPaddingLeft() - thumbOffset + (int) ((long) level(getProgress()) * Math.max(0, available) / 10000);
        int h = thumb.getIntrinsicHeight(), y = (getHeight() - h) / 2;
        thumb.setBounds(x, y, x + thumb.getIntrinsicWidth(), y + h);
        thumb.draw(canvas);
    }

    void onStartTrackingTouch() {}
    void onStopTrackingTouch() {}
    private void trackTouch(MotionEvent event) {
        int width = getWidth() - getPaddingLeft() - getPaddingRight();
        float scale = width > 0 ? Math.max(0, Math.min(1, (event.getX() - getPaddingLeft()) / width)) : 0;
        setProgressInternal(Math.round(getMin() + scale * (getMax() - getMin())), true);
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        if (!isEnabled()) return false;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: setPressed(true); onStartTrackingTouch(); trackTouch(event); if (getParent() != null) getParent().requestDisallowInterceptTouchEvent(true); break;
            case MotionEvent.ACTION_MOVE: trackTouch(event); break;
            case MotionEvent.ACTION_UP: trackTouch(event); onStopTrackingTouch(); setPressed(false); performClick(); break;
            case MotionEvent.ACTION_CANCEL: onStopTrackingTouch(); setPressed(false); break;
            default: break;
        }
        return true;
    }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (isEnabled()) {
            if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT || keyCode == KeyEvent.KEYCODE_MINUS) { setProgressInternal(getProgress() - keyProgressIncrement, true); return true; }
            if (keyCode == KeyEvent.KEYCODE_DPAD_RIGHT || keyCode == KeyEvent.KEYCODE_PLUS || keyCode == KeyEvent.KEYCODE_EQUALS) { setProgressInternal(getProgress() + keyProgressIncrement, true); return true; }
        }
        return super.onKeyDown(keyCode, event);
    }
    @Override public CharSequence getAccessibilityClassName() { return AbsSeekBar.class.getName(); }
}
