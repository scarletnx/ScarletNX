// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.os.Parcel;
import android.os.Parcelable;

/** A user's rating of a media item, in one of the rating styles. */
public final class Rating implements Parcelable {
    public static final int RATING_NONE = 0, RATING_HEART = 1, RATING_THUMB_UP_DOWN = 2, RATING_3_STARS = 3,
        RATING_4_STARS = 4, RATING_5_STARS = 5, RATING_PERCENTAGE = 6;
    private static final float RATING_NOT_RATED = -1f;

    private final int style;
    private final float value;

    private Rating(int style, float value) { this.style = style; this.value = value; }

    public static Rating newUnratedRating(int ratingStyle) {
        return ratingStyle >= RATING_HEART && ratingStyle <= RATING_PERCENTAGE ? new Rating(ratingStyle, RATING_NOT_RATED) : null;
    }
    public static Rating newHeartRating(boolean hasHeart) { return new Rating(RATING_HEART, hasHeart ? 1f : 0f); }
    public static Rating newThumbRating(boolean thumbIsUp) { return new Rating(RATING_THUMB_UP_DOWN, thumbIsUp ? 1f : 0f); }

    /** Null for a style that is not a star style, or a rating outside 0 to the style's number of stars. */
    public static Rating newStarRating(int starRatingStyle, float starRating) {
        float max = switch (starRatingStyle) {
            case RATING_3_STARS -> 3f;
            case RATING_4_STARS -> 4f;
            case RATING_5_STARS -> 5f;
            default -> -1f;
        };
        if (max < 0 || starRating < 0 || starRating > max) return null;
        return new Rating(starRatingStyle, starRating);
    }

    public static Rating newPercentageRating(float percent) {
        return percent < 0f || percent > 100f ? null : new Rating(RATING_PERCENTAGE, percent);
    }

    public boolean isRated() { return value >= 0f; }
    public int getRatingStyle() { return style; }
    public boolean hasHeart() { return style == RATING_HEART && value == 1f; }
    public boolean isThumbUp() { return style == RATING_THUMB_UP_DOWN && value == 1f; }
    public float getStarRating() {
        return (style == RATING_3_STARS || style == RATING_4_STARS || style == RATING_5_STARS) && isRated() ? value : -1f;
    }
    public float getPercentRating() { return style == RATING_PERCENTAGE && isRated() ? value : -1f; }

    @Override public String toString() { return "Rating:style=" + style + " rating=" + (value < 0f ? "unrated" : String.valueOf(value)); }
    @Override public int describeContents() { return style; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeInt(style); dest.writeFloat(value); }
    public static final Parcelable.Creator<Rating> CREATOR = new Parcelable.Creator<>() {
        @Override public Rating createFromParcel(Parcel in) { return new Rating(in.readInt(), in.readFloat()); }
        @Override public Rating[] newArray(int size) { return new Rating[size]; }
    };
}
