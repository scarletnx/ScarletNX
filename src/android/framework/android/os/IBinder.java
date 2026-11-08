// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.io.FileDescriptor;

/** A remote object's interface. In one process every binder is local. */
public interface IBinder {
    int FIRST_CALL_TRANSACTION = 0x00000001;
    int LAST_CALL_TRANSACTION = 0x00ffffff;
    int PING_TRANSACTION = ('_' << 24) | ('P' << 16) | ('N' << 8) | 'G';
    int DUMP_TRANSACTION = ('_' << 24) | ('D' << 16) | ('M' << 8) | 'P';
    int INTERFACE_TRANSACTION = ('_' << 24) | ('N' << 16) | ('T' << 8) | 'F';
    int FLAG_ONEWAY = 0x00000001;

    String getInterfaceDescriptor() throws RemoteException;

    boolean pingBinder();

    boolean isBinderAlive();

    IInterface queryLocalInterface(String descriptor);

    void dump(FileDescriptor fd, String[] args) throws RemoteException;

    void dumpAsync(FileDescriptor fd, String[] args) throws RemoteException;

    boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException;

    interface DeathRecipient {
        void binderDied();

        default void binderDied(IBinder who) { binderDied(); }
    }

    void linkToDeath(DeathRecipient recipient, int flags) throws RemoteException;

    boolean unlinkToDeath(DeathRecipient recipient, int flags);
}
