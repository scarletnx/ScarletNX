// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.opengl;

import java.nio.Buffer;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.DoubleBuffer;
import java.nio.FloatBuffer;
import java.nio.IntBuffer;
import java.nio.LongBuffer;
import java.nio.ShortBuffer;
import javax.microedition.khronos.opengles.GL11;

/**
 * GLES 1.1's Java calls, each the runtime's own GLES 1.1 function (hle_gles.c) by its name, with its arguments as
 * the native code's: params gives each one's kind in order, i an integer, f a float and p the pointer.
 */
final class Gl implements GL11 {
    static final Gl INSTANCE = new Gl();
    private static final long[] NO_INTS = {};
    private static final float[] NO_FLOATS = {};

    private Gl() {}

    /** the function's result (an int's, or a string's address for glGetString) */
    private static native long nativeCall(String name, String params, long[] ints, float[] floats, Object pointer, int byteOffset);
    private static native String nativeGetString(int name);

    private static long call(String name, String params, long[] ints, float[] floats) {
        return nativeCall(name, params, ints, floats, null, 0);
    }
    private static long ints(String name, long... ints) { return call(name, "iiiiiiiii".substring(0, ints.length), ints, NO_FLOATS); }
    private static void floats(String name, float... floats) { call(name, "ffffff".substring(0, floats.length), NO_INTS, floats); }

    private static int shift(Buffer b) {
        if (b instanceof ByteBuffer) return 0;
        if (b instanceof ShortBuffer || b instanceof CharBuffer) return 1;
        if (b instanceof LongBuffer || b instanceof DoubleBuffer) return 3;
        return 2;
    }
    /** a call whose last argument is a buffer's memory, from its position */
    private static long buffer(String name, String params, long[] ints, float[] floats, Buffer b) {
        if (b == null) return nativeCall(name, params, ints, floats, null, 0);
        Object base = b.isDirect() ? b : b.hasArray() ? b.array() : null;
        if (base == null) throw new IllegalArgumentException("a buffer must be direct or have an array");
        int offset = ((b.isDirect() ? 0 : b.arrayOffset()) + b.position()) << shift(b);
        return nativeCall(name, params, ints, floats, base, offset);
    }
    private static long array(String name, String params, long[] ints, Object array, int byteOffset) {
        if (array == null) throw new IllegalArgumentException("the array must not be null");
        return nativeCall(name, params, ints, NO_FLOATS, array, byteOffset);
    }

    @Override public void glActiveTexture(int texture) { ints("glActiveTexture", texture); }
    @Override public void glAlphaFunc(int func, float ref) { call("glAlphaFunc", "if", new long[] {func}, new float[] {ref}); }
    @Override public void glBindTexture(int target, int texture) { ints("glBindTexture", target, texture); }
    @Override public void glBlendFunc(int sfactor, int dfactor) { ints("glBlendFunc", sfactor, dfactor); }
    @Override public void glClear(int mask) { ints("glClear", mask); }
    @Override public void glClearColor(float r, float g, float b, float a) { floats("glClearColor", r, g, b, a); }
    @Override public void glClientActiveTexture(int texture) { ints("glClientActiveTexture", texture); }
    @Override public void glColor4f(float r, float g, float b, float a) { floats("glColor4f", r, g, b, a); }
    @Override public void glColor4x(int r, int g, int b, int a) { ints("glColor4x", r, g, b, a); }
    @Override public void glColor4ub(byte r, byte g, byte b, byte a) { ints("glColor4ub", r & 0xff, g & 0xff, b & 0xff, a & 0xff); }
    @Override public void glColorMask(boolean r, boolean g, boolean b, boolean a) {
        ints("glColorMask", r ? 1 : 0, g ? 1 : 0, b ? 1 : 0, a ? 1 : 0);
    }
    @Override public void glColorPointer(int size, int type, int stride, Buffer pointer) {
        buffer("glColorPointer", "iiip", new long[] {size, type, stride}, NO_FLOATS, pointer);
    }
    @Override public void glCullFace(int mode) { ints("glCullFace", mode); }
    @Override public void glDeleteTextures(int n, int[] textures, int offset) { array("glDeleteTextures", "ip", new long[] {n}, textures, offset * 4); }
    @Override public void glDeleteTextures(int n, IntBuffer textures) { buffer("glDeleteTextures", "ip", new long[] {n}, NO_FLOATS, textures); }
    @Override public void glDepthFunc(int func) { ints("glDepthFunc", func); }
    @Override public void glDepthMask(boolean flag) { ints("glDepthMask", flag ? 1 : 0); }
    @Override public void glDisable(int cap) { ints("glDisable", cap); }
    @Override public void glDisableClientState(int array) { ints("glDisableClientState", array); }
    @Override public void glDrawArrays(int mode, int first, int count) { ints("glDrawArrays", mode, first, count); }
    @Override public void glDrawElements(int mode, int count, int type, Buffer indices) {
        buffer("glDrawElements", "iiip", new long[] {mode, count, type}, NO_FLOATS, indices);
    }
    @Override public void glEnable(int cap) { ints("glEnable", cap); }
    @Override public void glEnableClientState(int array) { ints("glEnableClientState", array); }
    @Override public void glFinish() { ints("glFinish"); }
    @Override public void glFlush() { ints("glFlush"); }
    @Override public void glFrontFace(int mode) { ints("glFrontFace", mode); }
    @Override public void glGenTextures(int n, int[] textures, int offset) { array("glGenTextures", "ip", new long[] {n}, textures, offset * 4); }
    @Override public void glGenTextures(int n, IntBuffer textures) { buffer("glGenTextures", "ip", new long[] {n}, NO_FLOATS, textures); }
    @Override public int glGetError() { return (int) ints("glGetError"); }
    @Override public void glGetIntegerv(int pname, int[] params, int offset) { array("glGetIntegerv", "ip", new long[] {pname}, params, offset * 4); }
    @Override public void glGetIntegerv(int pname, IntBuffer params) { buffer("glGetIntegerv", "ip", new long[] {pname}, NO_FLOATS, params); }
    @Override public String glGetString(int name) { return nativeGetString(name); }
    @Override public void glHint(int target, int mode) { ints("glHint", target, mode); }
    @Override public void glLoadIdentity() { ints("glLoadIdentity"); }
    @Override public void glLoadMatrixf(float[] m, int offset) { array("glLoadMatrixf", "p", NO_INTS, m, offset * 4); }
    @Override public void glLoadMatrixf(FloatBuffer m) { buffer("glLoadMatrixf", "p", NO_INTS, NO_FLOATS, m); }
    @Override public void glMatrixMode(int mode) { ints("glMatrixMode", mode); }
    @Override public void glMultMatrixf(float[] m, int offset) { array("glMultMatrixf", "p", NO_INTS, m, offset * 4); }
    @Override public void glMultMatrixf(FloatBuffer m) { buffer("glMultMatrixf", "p", NO_INTS, NO_FLOATS, m); }
    @Override public void glOrthof(float l, float r, float b, float t, float n, float f) { floats("glOrthof", l, r, b, t, n, f); }
    @Override public void glOrthox(int l, int r, int b, int t, int n, int f) { ints("glOrthox", l, r, b, t, n, f); }
    @Override public void glPixelStorei(int pname, int param) { ints("glPixelStorei", pname, param); }
    @Override public void glPopMatrix() { ints("glPopMatrix"); }
    @Override public void glPushMatrix() { ints("glPushMatrix"); }
    @Override public void glReadPixels(int x, int y, int width, int height, int format, int type, Buffer pixels) {
        buffer("glReadPixels", "iiiiiip", new long[] {x, y, width, height, format, type}, NO_FLOATS, pixels);
    }
    @Override public void glRotatef(float angle, float x, float y, float z) { floats("glRotatef", angle, x, y, z); }
    @Override public void glScalef(float x, float y, float z) { floats("glScalef", x, y, z); }
    @Override public void glScissor(int x, int y, int width, int height) { ints("glScissor", x, y, width, height); }
    @Override public void glShadeModel(int mode) { ints("glShadeModel", mode); }
    @Override public void glTexCoordPointer(int size, int type, int stride, Buffer pointer) {
        buffer("glTexCoordPointer", "iiip", new long[] {size, type, stride}, NO_FLOATS, pointer);
    }
    @Override public void glTexEnvf(int target, int pname, float param) { call("glTexEnvf", "iif", new long[] {target, pname}, new float[] {param}); }
    @Override public void glTexEnvi(int target, int pname, int param) { ints("glTexEnvi", target, pname, param); }
    @Override public void glTexImage2D(int target, int level, int internalformat, int width, int height, int border,
                                       int format, int type, Buffer pixels) {
        buffer("glTexImage2D", "iiiiiiiip", new long[] {target, level, internalformat, width, height, border, format, type},
               NO_FLOATS, pixels);
    }
    @Override public void glTexParameterf(int target, int pname, float param) {
        call("glTexParameterf", "iif", new long[] {target, pname}, new float[] {param});
    }
    @Override public void glTexParameteri(int target, int pname, int param) { ints("glTexParameteri", target, pname, param); }
    @Override public void glTexParameteriv(int target, int pname, int[] params, int offset) {
        array("glTexParameteriv", "iip", new long[] {target, pname}, params, offset * 4);
    }
    @Override public void glTexSubImage2D(int target, int level, int xoffset, int yoffset, int width, int height,
                                          int format, int type, Buffer pixels) {
        buffer("glTexSubImage2D", "iiiiiiiip", new long[] {target, level, xoffset, yoffset, width, height, format, type},
               NO_FLOATS, pixels);
    }
    @Override public void glTranslatef(float x, float y, float z) { floats("glTranslatef", x, y, z); }
    @Override public void glVertexPointer(int size, int type, int stride, Buffer pointer) {
        buffer("glVertexPointer", "iiip", new long[] {size, type, stride}, NO_FLOATS, pointer);
    }
    @Override public void glViewport(int x, int y, int width, int height) { ints("glViewport", x, y, width, height); }
}
