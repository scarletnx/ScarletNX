// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A drag and drop event. */
public class DragEvent implements android.os.Parcelable {
    public static final int ACTION_DRAG_STARTED = 1, ACTION_DRAG_LOCATION = 2, ACTION_DROP = 3, ACTION_DRAG_ENDED = 4,
        ACTION_DRAG_ENTERED = 5, ACTION_DRAG_EXITED = 6;
    int action;
    float x, y;
    android.content.ClipData clipData;
    android.content.ClipDescription description;

    public int getAction() { return action; }
    public float getX() { return x; }
    public float getY() { return y; }
    public android.content.ClipData getClipData() { return clipData; }
    public android.content.ClipDescription getClipDescription() { return description; }
    public Object getLocalState() { return null; }
    public boolean getResult() { return false; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
