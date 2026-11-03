// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package dalvik.system;

/**
 * A loader that looks in its own DEX path before its parent (after the boot classes). Its own path gives no classes
 * here (BaseDexClassLoader), so the order is the parent's.
 */
public final class DelegateLastClassLoader extends PathClassLoader {
    public DelegateLastClassLoader(String dexPath, ClassLoader parent) { super(dexPath, parent); }
    public DelegateLastClassLoader(String dexPath, String librarySearchPath, ClassLoader parent) {
        super(dexPath, librarySearchPath, parent);
    }
    public DelegateLastClassLoader(String dexPath, String librarySearchPath, ClassLoader parent,
                                   boolean delegateResourceLoading) {
        super(dexPath, librarySearchPath, parent);
    }
}
