// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.os;

import java.io.FileDescriptor;

/** A local binder: a transaction is a direct call of onTransact, on the caller's thread. */
public class Binder implements IBinder {
    private IInterface owner;
    private String descriptor;

    public Binder() {}

    public Binder(String descriptor) { this.descriptor = descriptor; }

    public static final int getCallingPid() { return Process.myPid(); }
    public static final int getCallingUid() { return Process.myUid(); }
    public static final UserHandle getCallingUserHandle() { return UserHandle.OWNER; }
    public static final long clearCallingIdentity() { return 0; }
    public static final void restoreCallingIdentity(long token) {}
    public static final void flushPendingCommands() {}
    public static final boolean isProxy(IInterface iface) { return false; }

    public void attachInterface(IInterface owner, String descriptor) {
        this.owner = owner;
        this.descriptor = descriptor;
    }

    @Override public String getInterfaceDescriptor() { return descriptor; }
    @Override public boolean pingBinder() { return true; }
    @Override public boolean isBinderAlive() { return true; }
    @Override public IInterface queryLocalInterface(String descriptor) {
        return this.descriptor != null && this.descriptor.equals(descriptor) ? owner : null;
    }
    @Override public void dump(FileDescriptor fd, String[] args) {}
    @Override public void dumpAsync(FileDescriptor fd, String[] args) {}

    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        if (code == INTERFACE_TRANSACTION) {
            reply.writeString(getInterfaceDescriptor());
            return true;
        }
        return false;
    }

    @Override
    public final boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        if (data != null) data.setDataPosition(0);
        boolean r = onTransact(code, data, reply, flags);
        if (reply != null) reply.setDataPosition(0);
        return r;
    }

    @Override public void linkToDeath(DeathRecipient recipient, int flags) {}
    @Override public boolean unlinkToDeath(DeathRecipient recipient, int flags) { return true; }
}
