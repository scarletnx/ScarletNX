// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package javax.microedition.khronos.opengles;

/** GLES 1.1's Java interface: GL10's, and the calls that 1.1 adds that the runtime has. */
public interface GL11 extends GL10 {
    void glColor4ub(byte red, byte green, byte blue, byte alpha);
    void glTexEnvi(int target, int pname, int param);
    void glTexParameteri(int target, int pname, int param);
    void glTexParameteriv(int target, int pname, int[] params, int offset);
}
