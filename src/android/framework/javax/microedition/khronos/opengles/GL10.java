// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package javax.microedition.khronos.opengles;

import java.nio.Buffer;
import java.nio.FloatBuffer;
import java.nio.IntBuffer;

/**
 * GLES 1.0's Java interface (JSR 239), as GLSurfaceView's Renderer receives it: the calls that the runtime's GLES 1.1
 * has (src/android/bridge/hle_gles.c), on the calling thread's current context. A pointer is a direct buffer, or an
 * array (a buffer's or one given with an offset), read from its position.
 */
public interface GL10 extends GL {
    void glActiveTexture(int texture);
    void glAlphaFunc(int func, float ref);
    void glBindTexture(int target, int texture);
    void glBlendFunc(int sfactor, int dfactor);
    void glClear(int mask);
    void glClearColor(float red, float green, float blue, float alpha);
    void glClientActiveTexture(int texture);
    void glColor4f(float red, float green, float blue, float alpha);
    void glColor4x(int red, int green, int blue, int alpha);
    void glColorMask(boolean red, boolean green, boolean blue, boolean alpha);
    void glColorPointer(int size, int type, int stride, Buffer pointer);
    void glCullFace(int mode);
    void glDeleteTextures(int n, int[] textures, int offset);
    void glDeleteTextures(int n, IntBuffer textures);
    void glDepthFunc(int func);
    void glDepthMask(boolean flag);
    void glDisable(int cap);
    void glDisableClientState(int array);
    void glDrawArrays(int mode, int first, int count);
    void glDrawElements(int mode, int count, int type, Buffer indices);
    void glEnable(int cap);
    void glEnableClientState(int array);
    void glFinish();
    void glFlush();
    void glFrontFace(int mode);
    void glGenTextures(int n, int[] textures, int offset);
    void glGenTextures(int n, IntBuffer textures);
    int glGetError();
    void glGetIntegerv(int pname, int[] params, int offset);
    void glGetIntegerv(int pname, IntBuffer params);
    String glGetString(int name);
    void glHint(int target, int mode);
    void glLoadIdentity();
    void glLoadMatrixf(float[] m, int offset);
    void glLoadMatrixf(FloatBuffer m);
    void glMatrixMode(int mode);
    void glMultMatrixf(float[] m, int offset);
    void glMultMatrixf(FloatBuffer m);
    void glOrthof(float left, float right, float bottom, float top, float zNear, float zFar);
    void glOrthox(int left, int right, int bottom, int top, int zNear, int zFar);
    void glPixelStorei(int pname, int param);
    void glPopMatrix();
    void glPushMatrix();
    void glReadPixels(int x, int y, int width, int height, int format, int type, Buffer pixels);
    void glRotatef(float angle, float x, float y, float z);
    void glScalef(float x, float y, float z);
    void glScissor(int x, int y, int width, int height);
    void glShadeModel(int mode);
    void glTexCoordPointer(int size, int type, int stride, Buffer pointer);
    void glTexEnvf(int target, int pname, float param);
    void glTexImage2D(int target, int level, int internalformat, int width, int height, int border, int format,
                      int type, Buffer pixels);
    void glTexParameterf(int target, int pname, float param);
    void glTexSubImage2D(int target, int level, int xoffset, int yoffset, int width, int height, int format, int type,
                         Buffer pixels);
    void glTranslatef(float x, float y, float z);
    void glVertexPointer(int size, int type, int stride, Buffer pointer);
    void glViewport(int x, int y, int width, int height);
}
