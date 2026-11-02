// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A Porter-Duff transfer mode. */
public class PorterDuffXfermode extends Xfermode {
    public PorterDuffXfermode(PorterDuff.Mode mode) { this.mode = BlendMode.fromPorterDuff(mode); }
}
