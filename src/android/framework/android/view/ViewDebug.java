// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/** Debug annotations of views. No debugger reads them here. */
public class ViewDebug {
    @Deprecated public static final boolean TRACE_HIERARCHY = false, TRACE_RECYCLER = false;

    @Target({ElementType.FIELD, ElementType.METHOD})
    @Retention(RetentionPolicy.RUNTIME)
    public @interface ExportedProperty {
        boolean resolveId() default false;
        IntToString[] mapping() default {};
        IntToString[] indexMapping() default {};
        FlagToString[] flagMapping() default {};
        boolean deepExport() default false;
        String prefix() default "";
        String category() default "";
        boolean formatToHexString() default false;
        boolean hasAdjacentMapping() default false;
    }

    @Target({ElementType.TYPE})
    @Retention(RetentionPolicy.RUNTIME)
    public @interface IntToString {
        int from();
        String to();
    }

    @Target({ElementType.TYPE})
    @Retention(RetentionPolicy.RUNTIME)
    public @interface FlagToString {
        int mask();
        int equals();
        String name();
        boolean outputIf() default true;
    }

    @Target({ElementType.FIELD, ElementType.METHOD})
    @Retention(RetentionPolicy.RUNTIME)
    public @interface CapturedViewProperty {
        boolean retrieveReturn() default false;
    }

    public static void dumpCapturedView(String tag, Object view) {}
}
