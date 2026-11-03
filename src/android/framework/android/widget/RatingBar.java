// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;

/** A row of stars: a seek bar whose progress counts steps of a star. */
public class RatingBar extends AbsSeekBar {
    private int numStars = 5;
    private float stepSize = 0.5f;
    private boolean indicator;
    private OnRatingBarChangeListener listener;

    public interface OnRatingBarChangeListener { void onRatingChanged(RatingBar ratingBar, float rating, boolean fromUser); }

    public RatingBar(Context context) { this(context, null); }
    public RatingBar(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public RatingBar(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public RatingBar(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        if (attrs != null) {
            String ns = "http://schemas.android.com/apk/res/android";
            numStars = Math.max(1, attrs.getAttributeIntValue(ns, "numStars", numStars));
            float step = attrs.getAttributeFloatValue(ns, "stepSize", stepSize);
            if (step > 0) stepSize = step;
            indicator = attrs.getAttributeBooleanValue(ns, "isIndicator", false);
        }
        setMax(Math.round(numStars / stepSize));
        if (attrs != null) setRating(attrs.getAttributeFloatValue("http://schemas.android.com/apk/res/android", "rating", 0));
    }

    public void setOnRatingBarChangeListener(OnRatingBarChangeListener listener) { this.listener = listener; }
    public OnRatingBarChangeListener getOnRatingBarChangeListener() { return listener; }
    public void setIsIndicator(boolean isIndicator) { indicator = isIndicator; setFocusable(!isIndicator); }
    public boolean isIndicator() { return indicator; }
    public void setNumStars(int numStars) {
        if (numStars <= 0) return;
        float rating = getRating();
        this.numStars = numStars;
        setMax(Math.round(numStars / stepSize));
        setRating(rating);
        requestLayout();
    }
    public int getNumStars() { return numStars; }
    public void setRating(float rating) { setProgress(Math.round(rating / stepSize)); }
    public float getRating() { return getProgress() * stepSize; }
    public void setStepSize(float stepSize) {
        if (stepSize <= 0) return;
        float rating = getRating();
        this.stepSize = stepSize;
        setMax(Math.round(numStars / stepSize));
        setRating(rating);
    }
    public float getStepSize() { return stepSize; }
    @Override void onProgressRefresh(boolean fromUser) { if (listener != null) listener.onRatingChanged(this, getRating(), fromUser); }
    @Override public boolean onTouchEvent(android.view.MotionEvent event) { return !indicator && super.onTouchEvent(event); }
    @Override public CharSequence getAccessibilityClassName() { return RatingBar.class.getName(); }
}
