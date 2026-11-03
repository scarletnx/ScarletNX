// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;

/** A two-state switch: a compound button with a thumb and a track. */
public class Switch extends CompoundButton {
    private CharSequence textOn, textOff;
    private boolean showText, splitTrack;
    private Drawable thumb, track;
    private ColorStateList thumbTint, trackTint;
    private int minWidth, thumbPadding, switchPadding;

    public Switch(Context context) { this(context, null); }
    public Switch(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public Switch(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public Switch(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public CharSequence getTextOn() { return textOn; }
    public void setTextOn(CharSequence textOn) { this.textOn = textOn; requestLayout(); }
    public CharSequence getTextOff() { return textOff; }
    public void setTextOff(CharSequence textOff) { this.textOff = textOff; requestLayout(); }
    public void setShowText(boolean showText) { this.showText = showText; requestLayout(); }
    public boolean getShowText() { return showText; }
    public void setSplitTrack(boolean splitTrack) { this.splitTrack = splitTrack; }
    public boolean getSplitTrack() { return splitTrack; }
    public void setThumbDrawable(Drawable thumb) { this.thumb = thumb; requestLayout(); }
    public void setThumbResource(int resId) { setThumbDrawable(getContext().getDrawable(resId)); }
    public Drawable getThumbDrawable() { return thumb; }
    public void setTrackDrawable(Drawable track) { this.track = track; requestLayout(); }
    public void setTrackResource(int resId) { setTrackDrawable(getContext().getDrawable(resId)); }
    public Drawable getTrackDrawable() { return track; }
    public void setThumbTintList(ColorStateList tint) { thumbTint = tint; if (thumb != null) thumb.setTintList(tint); }
    public ColorStateList getThumbTintList() { return thumbTint; }
    public void setTrackTintList(ColorStateList tint) { trackTint = tint; if (track != null) track.setTintList(tint); }
    public ColorStateList getTrackTintList() { return trackTint; }
    public void setSwitchMinWidth(int pixels) { minWidth = pixels; requestLayout(); }
    public int getSwitchMinWidth() { return minWidth; }
    public void setThumbTextPadding(int pixels) { thumbPadding = pixels; requestLayout(); }
    public int getThumbTextPadding() { return thumbPadding; }
    public void setSwitchPadding(int pixels) { switchPadding = pixels; requestLayout(); }
    public int getSwitchPadding() { return switchPadding; }
    @Override public CharSequence getAccessibilityClassName() { return Switch.class.getName(); }
}
