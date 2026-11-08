// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/**
 * A Handler that another component sends messages to. Its binder carries the Handler, so a Messenger made from
 * that binder (as a client makes one from a service's onBind) sends to the same Handler.
 */
public final class Messenger implements Parcelable {
    private static final class MessengerBinder extends Binder {
        final Handler target;
        MessengerBinder(Handler target) { super(DESCRIPTOR); this.target = target; }
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != SEND) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            target.sendMessage((Message) data.readValue(null));
            return true;
        }
    }

    private static final String DESCRIPTOR = "android.os.IMessenger";
    private static final int SEND = IBinder.FIRST_CALL_TRANSACTION;

    private final IBinder binder;

    public Messenger(Handler target) { binder = new MessengerBinder(target); }
    public Messenger(IBinder target) { binder = target; }

    /** To the Handler directly, or as a one-way transaction to a binder that another Messenger did not make. */
    public void send(Message message) throws RemoteException {
        if (binder instanceof MessengerBinder mb) {
            mb.target.sendMessage(message);
            return;
        }
        Parcel data = Parcel.obtain();
        data.writeInterfaceToken(DESCRIPTOR);
        data.writeValue(message);
        binder.transact(SEND, data, null, IBinder.FLAG_ONEWAY);
    }
    public IBinder getBinder() { return binder; }
    @Override public boolean equals(Object o) { return o instanceof Messenger m && m.binder.equals(binder); }
    @Override public int hashCode() { return binder.hashCode(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) { out.writeStrongBinder(binder); }
    public static void writeMessengerOrNullToParcel(Messenger messenger, Parcel out) { out.writeStrongBinder(messenger != null ? messenger.binder : null); }
    public static Messenger readMessengerOrNullFromParcel(Parcel in) { IBinder b = in.readStrongBinder(); return b != null ? new Messenger(b) : null; }
    public static final Parcelable.Creator<Messenger> CREATOR = new Parcelable.Creator<>() {
        @Override public Messenger createFromParcel(Parcel in) { IBinder b = in.readStrongBinder(); return b != null ? new Messenger(b) : null; }
        @Override public Messenger[] newArray(int size) { return new Messenger[size]; }
    };
}
