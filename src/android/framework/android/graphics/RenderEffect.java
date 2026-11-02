// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** An effect on a RenderNode's drawing (blurs and the like: drawn without it here). */
public final class RenderEffect {
    private RenderEffect() {}
    public static RenderEffect createBlurEffect(float radiusX, float radiusY, Shader.TileMode edgeTreatment) { return new RenderEffect(); }
    public static RenderEffect createBlurEffect(float radiusX, float radiusY, RenderEffect input, Shader.TileMode edgeTreatment) { return new RenderEffect(); }
    public static RenderEffect createOffsetEffect(float offsetX, float offsetY) { return new RenderEffect(); }
    public static RenderEffect createOffsetEffect(float offsetX, float offsetY, RenderEffect input) { return new RenderEffect(); }
    public static RenderEffect createColorFilterEffect(ColorFilter colorFilter) { return new RenderEffect(); }
    public static RenderEffect createColorFilterEffect(ColorFilter colorFilter, RenderEffect renderEffect) { return new RenderEffect(); }
    public static RenderEffect createChainEffect(RenderEffect outer, RenderEffect inner) { return new RenderEffect(); }
    public static RenderEffect createShaderEffect(Shader shader) { return new RenderEffect(); }
}
