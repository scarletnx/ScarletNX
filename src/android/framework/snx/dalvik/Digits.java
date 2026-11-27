// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.dalvik;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

/**
 * A double's and a float's digits as ART prints them: OpenJDK's before JDK 19, which libcore's FloatingDecimal still
 * is (1.0E23 is 9.999999999999999E22, 2.82879384806159E17 is 2.82879384806159008E17). The JDK prints the shortest
 * digits that read back since JDK 19 (1.0E23), so the translator sends Double.toString, Float.toString and the calls
 * that print a number as they do here (translate.c's remaps), and the framework's own printing calls these. The JDK
 * keeps the old digits in jdk.internal.math.FloatingDecimal, which the driver exports to the framework
 * (runtime/jvm.c). A box printed through Object.toString, String.valueOf(Object) or a Formatter's %s is the JDK's.
 */
public final class Digits {
    private Digits() {}

    private static final MethodHandle DOUBLE, FLOAT;
    static {
        try {
            Class<?> c = Class.forName("jdk.internal.math.FloatingDecimal");
            MethodHandles.Lookup lookup = MethodHandles.lookup();
            DOUBLE = lookup.findStatic(c, "toJavaFormatString", MethodType.methodType(String.class, double.class));
            FLOAT = lookup.findStatic(c, "toJavaFormatString", MethodType.methodType(String.class, float.class));
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(new IllegalStateException(
                "ART's digits of a double are jdk.internal.math.FloatingDecimal.toJavaFormatString's, which this JDK ("
                    + System.getProperty("java.version") + ") does not give the framework: " + e, e));
        }
    }

    public static String toString(double d) {
        try {
            return (String) DOUBLE.invokeExact(d);
        } catch (Throwable t) {
            throw new IllegalStateException("FloatingDecimal.toJavaFormatString(" + d + ") threw " + t, t);
        }
    }

    public static String toString(float f) {
        try {
            return (String) FLOAT.invokeExact(f);
        } catch (Throwable t) {
            throw new IllegalStateException("FloatingDecimal.toJavaFormatString(" + f + "f) threw " + t, t);
        }
    }

    /** Double.toString() and Float.toString() of a box, which the translator sends here with the box first */
    public static String toString(Double d) {
        if (d == null) throw nullReceiver("java.lang.String java.lang.Double.toString()");
        return toString(d.doubleValue());
    }

    public static String toString(Float f) {
        if (f == null) throw nullReceiver("java.lang.String java.lang.Float.toString()");
        return toString(f.floatValue());
    }

    /** A number as String.valueOf(Object) prints it on ART: a Double's and a Float's digits are ART's */
    public static String valueOf(Object o) {
        if (o instanceof Double d) return toString(d.doubleValue());
        if (o instanceof Float f) return toString(f.floatValue());
        return String.valueOf(o);
    }

    // StringBuilder, StringBuffer, PrintStream and PrintWriter: the receiver first, as the translator passes it
    public static StringBuilder append(StringBuilder b, double d) {
        if (b == null) throw nullReceiver("java.lang.StringBuilder java.lang.StringBuilder.append(double)");
        return b.append(toString(d));
    }

    public static StringBuilder append(StringBuilder b, float f) {
        if (b == null) throw nullReceiver("java.lang.StringBuilder java.lang.StringBuilder.append(float)");
        return b.append(toString(f));
    }

    public static StringBuffer append(StringBuffer b, double d) {
        if (b == null) throw nullReceiver("java.lang.StringBuffer java.lang.StringBuffer.append(double)");
        return b.append(toString(d));
    }

    public static StringBuffer append(StringBuffer b, float f) {
        if (b == null) throw nullReceiver("java.lang.StringBuffer java.lang.StringBuffer.append(float)");
        return b.append(toString(f));
    }

    public static StringBuilder insert(StringBuilder b, int offset, double d) {
        if (b == null) throw nullReceiver("java.lang.StringBuilder java.lang.StringBuilder.insert(int, double)");
        return b.insert(offset, toString(d));
    }

    public static StringBuilder insert(StringBuilder b, int offset, float f) {
        if (b == null) throw nullReceiver("java.lang.StringBuilder java.lang.StringBuilder.insert(int, float)");
        return b.insert(offset, toString(f));
    }

    public static StringBuffer insert(StringBuffer b, int offset, double d) {
        if (b == null) throw nullReceiver("java.lang.StringBuffer java.lang.StringBuffer.insert(int, double)");
        return b.insert(offset, toString(d));
    }

    public static StringBuffer insert(StringBuffer b, int offset, float f) {
        if (b == null) throw nullReceiver("java.lang.StringBuffer java.lang.StringBuffer.insert(int, float)");
        return b.insert(offset, toString(f));
    }

    public static void print(java.io.PrintStream s, double d) {
        if (s == null) throw nullReceiver("void java.io.PrintStream.print(double)");
        s.print(toString(d));
    }

    public static void print(java.io.PrintStream s, float f) {
        if (s == null) throw nullReceiver("void java.io.PrintStream.print(float)");
        s.print(toString(f));
    }

    public static void println(java.io.PrintStream s, double d) {
        if (s == null) throw nullReceiver("void java.io.PrintStream.println(double)");
        s.println(toString(d));
    }

    public static void println(java.io.PrintStream s, float f) {
        if (s == null) throw nullReceiver("void java.io.PrintStream.println(float)");
        s.println(toString(f));
    }

    public static void print(java.io.PrintWriter w, double d) {
        if (w == null) throw nullReceiver("void java.io.PrintWriter.print(double)");
        w.print(toString(d));
    }

    public static void print(java.io.PrintWriter w, float f) {
        if (w == null) throw nullReceiver("void java.io.PrintWriter.print(float)");
        w.print(toString(f));
    }

    public static void println(java.io.PrintWriter w, double d) {
        if (w == null) throw nullReceiver("void java.io.PrintWriter.println(double)");
        w.println(toString(d));
    }

    public static void println(java.io.PrintWriter w, float f) {
        if (w == null) throw nullReceiver("void java.io.PrintWriter.println(float)");
        w.println(toString(f));
    }

    /** ART's message for a call on null, which the call that came here would have thrown */
    private static NullPointerException nullReceiver(String method) {
        return new NullPointerException("Attempt to invoke virtual method '" + method + "' on a null object reference");
    }
}
