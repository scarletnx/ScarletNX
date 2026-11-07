// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.os;

/** A message of a Handler: its what and arguments, or a callback, with the time it is due. */
public final class Message implements Parcelable {
    public int what;
    public int arg1;
    public int arg2;
    public Object obj;
    public Messenger replyTo;
    public int sendingUid = -1;
    long when;
    Bundle data;
    Handler target;
    Runnable callback;
    Message next;
    boolean asynchronous;
    boolean inUse;

    public Message() {}

    public static Message obtain() {
        return new Message();
    }

    public static Message obtain(Message orig) {
        Message m = obtain();
        m.what = orig.what;
        m.arg1 = orig.arg1;
        m.arg2 = orig.arg2;
        m.obj = orig.obj;
        m.replyTo = orig.replyTo;
        m.sendingUid = orig.sendingUid;
        if (orig.data != null) m.data = new Bundle(orig.data);
        m.target = orig.target;
        m.callback = orig.callback;
        return m;
    }

    public static Message obtain(Handler h) {
        Message m = obtain();
        m.target = h;
        return m;
    }

    public static Message obtain(Handler h, Runnable callback) {
        Message m = obtain(h);
        m.callback = callback;
        return m;
    }

    public static Message obtain(Handler h, int what) {
        Message m = obtain(h);
        m.what = what;
        return m;
    }

    public static Message obtain(Handler h, int what, Object obj) {
        Message m = obtain(h, what);
        m.obj = obj;
        return m;
    }

    public static Message obtain(Handler h, int what, int arg1, int arg2) {
        Message m = obtain(h, what);
        m.arg1 = arg1;
        m.arg2 = arg2;
        return m;
    }

    public static Message obtain(Handler h, int what, int arg1, int arg2, Object obj) {
        Message m = obtain(h, what, arg1, arg2);
        m.obj = obj;
        return m;
    }

    public void recycle() {}

    public long getWhen() { return when; }
    public void setTarget(Handler target) { this.target = target; }
    public Handler getTarget() { return target; }
    public Runnable getCallback() { return callback; }
    public Bundle getData() {
        if (data == null) data = new Bundle();
        return data;
    }
    public Bundle peekData() { return data; }
    public void setData(Bundle data) { this.data = data; }
    public void sendToTarget() { target.sendMessage(this); }
    public boolean isAsynchronous() { return asynchronous; }
    public void setAsynchronous(boolean async) { asynchronous = async; }
    public void copyFrom(Message o) {
        what = o.what; arg1 = o.arg1; arg2 = o.arg2; obj = o.obj; replyTo = o.replyTo; sendingUid = o.sendingUid;
        data = o.data != null ? new Bundle(o.data) : null;
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(what); dest.writeInt(arg1); dest.writeInt(arg2); dest.writeBundle(data);
    }

    @Override
    public String toString() {
        return "{ when=" + (when - SystemClock.uptimeMillis()) + "ms what=" + what + " target="
            + (target != null ? target.getClass().getName() : null) + (callback != null ? " callback=" + callback : "")
            + " }";
    }

    public static final Parcelable.Creator<Message> CREATOR = new Parcelable.Creator<>() {
        @Override public Message createFromParcel(Parcel in) {
            Message m = obtain();
            m.what = in.readInt(); m.arg1 = in.readInt(); m.arg2 = in.readInt(); m.data = in.readBundle(null);
            return m;
        }
        @Override public Message[] newArray(int size) { return new Message[size]; }
    };
}
