// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** An input event: a key or a motion. */
public abstract class InputEvent implements android.os.Parcelable {
    private static int nextSeq;
    private final int seq;

    InputEvent() { synchronized (InputEvent.class) { seq = nextSeq++; } }

    public abstract int getDeviceId();
    public final InputDevice getDevice() { return InputDevice.getDevice(getDeviceId()); }
    public abstract int getSource();
    public abstract void setSource(int source);
    public boolean isFromSource(int source) { return (getSource() & source) == source; }
    public abstract long getEventTime();
    public abstract long getEventTimeNanos();
    public void recycle() {}
    public int getSequenceNumber() { return seq; }
    public int getDisplayId() { return 0; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
