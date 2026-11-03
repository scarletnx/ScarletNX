// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media.session;

import android.os.Process;

/** The media sessions of the device: those of the app, as no other app runs. */
public final class MediaSessionManager {
    public MediaSessionManager() {}

    /** The app trusts itself; no other app is here. */
    public boolean isTrustedForMediaControl(RemoteUserInfo userInfo) {
        if (userInfo == null) throw new IllegalArgumentException("userInfo may not be null");
        return userInfo.getUid() == Process.myUid();
    }

    public static final class RemoteUserInfo {
        private final String packageName;
        private final int pid, uid;

        public RemoteUserInfo(String packageName, int pid, int uid) {
            if (packageName == null) throw new IllegalArgumentException("packageName shouldn't be null");
            if (packageName.isEmpty()) throw new IllegalArgumentException("packageName shouldn't be empty");
            this.packageName = packageName; this.pid = pid; this.uid = uid;
        }

        public String getPackageName() { return packageName; }
        public int getPid() { return pid; }
        public int getUid() { return uid; }

        @Override public boolean equals(Object o) {
            return o instanceof RemoteUserInfo r && packageName.equals(r.packageName) && pid == r.pid && uid == r.uid;
        }
        @Override public int hashCode() { return java.util.Objects.hash(packageName, pid, uid); }
    }
}
