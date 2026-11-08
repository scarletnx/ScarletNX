// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package dalvik.system;

/** The libcore view of the call stack */
public final class VMStack {
    private VMStack() {}

    private static final StackWalker walker = StackWalker.getInstance(StackWalker.Option.RETAIN_CLASS_REFERENCE);

    /** The class of the caller of the caller of the method that calls this one, or null at the bottom of the stack */
    public static Class<?> getStackClass2() {
        return walker.walk(s -> s.skip(3).findFirst().map(StackWalker.StackFrame::getDeclaringClass).orElse(null));
    }
    /** The class of the caller of the method that calls this one */
    public static Class<?> getStackClass1() {
        return walker.walk(s -> s.skip(2).findFirst().map(StackWalker.StackFrame::getDeclaringClass).orElse(null));
    }
}
