// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** StrictMode: no policy ever reports, so a policy only remembers itself. */
public final class StrictMode {
    private static final ThreadLocal<ThreadPolicy> threadPolicy = ThreadLocal.withInitial(() -> ThreadPolicy.LAX);
    private static VmPolicy vmPolicy = VmPolicy.LAX;

    private StrictMode() {}

    public static final class ThreadPolicy {
        public static final ThreadPolicy LAX = new ThreadPolicy();

        public static final class Builder {
            public Builder() {}
            public Builder(ThreadPolicy policy) {}
            public Builder detectAll() { return this; }
            public Builder permitAll() { return this; }
            public Builder detectNetwork() { return this; }
            public Builder permitNetwork() { return this; }
            public Builder detectDiskReads() { return this; }
            public Builder permitDiskReads() { return this; }
            public Builder detectDiskWrites() { return this; }
            public Builder permitDiskWrites() { return this; }
            public Builder detectCustomSlowCalls() { return this; }
            public Builder penaltyLog() { return this; }
            public Builder penaltyDeath() { return this; }
            public Builder penaltyDialog() { return this; }
            public ThreadPolicy build() { return new ThreadPolicy(); }
        }
    }

    public static final class VmPolicy {
        public static final VmPolicy LAX = new VmPolicy();

        public static final class Builder {
            public Builder() {}
            public Builder(VmPolicy policy) {}
            public Builder detectAll() { return this; }
            public Builder detectLeakedClosableObjects() { return this; }
            public Builder penaltyLog() { return this; }
            public Builder penaltyDeath() { return this; }
            public VmPolicy build() { return new VmPolicy(); }
        }
    }

    public static void setThreadPolicy(ThreadPolicy policy) { threadPolicy.set(policy); }
    public static ThreadPolicy getThreadPolicy() { return threadPolicy.get(); }
    public static ThreadPolicy allowThreadDiskWrites() { return threadPolicy.get(); }
    public static ThreadPolicy allowThreadDiskReads() { return threadPolicy.get(); }
    public static void setVmPolicy(VmPolicy policy) { vmPolicy = policy; }
    public static VmPolicy getVmPolicy() { return vmPolicy; }
    public static void enableDefaults() {}
    public static void noteSlowCall(String name) {}
}
