// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import snx.android.hwui.DisplayList;

/**
 * A recorded display list with its properties: position, transform, alpha, clip and outline. A frame flattens the
 * tree of nodes (snx.android.hwui.Frame) for the renderer.
 */
public final class RenderNode {
    private final String name;
    private DisplayList list;
    private DisplayList recording;
    private RecordingCanvas canvas;
    private int left, top, right, bottom;
    private float alpha = 1, translationX, translationY, translationZ, elevation, scaleX = 1, scaleY = 1, rotationX,
        rotationY, rotationZ, pivotX, pivotY, cameraDistance = 8;
    private boolean pivotSet, clipToBounds = true, clipToOutline, overlapping = true, compositing, forceDark = true;
    private final Outline outline = new Outline();
    private final Rect clipRect = new Rect();
    private boolean hasClipRect;
    private int ambientShadow = 0xff000000, spotShadow = 0xff000000;
    private long uniqueId;
    private static long nextId = 1;
    private Matrix animationMatrix;
    private RenderEffect renderEffect;
    private PositionUpdateListener positionListener;

    public interface PositionUpdateListener {
        void positionChanged(long frameNumber, int left, int top, int right, int bottom);

        default void positionChanged(long frameNumber, int left, int top, int right, int bottom, int clipLeft,
                                     int clipTop, int clipRight, int clipBottom) {
            positionChanged(frameNumber, left, top, right, bottom);
        }

        void positionLost(long frameNumber);
    }

    public RenderNode(String name) {
        this.name = name;
        synchronized (RenderNode.class) { uniqueId = nextId++; }
    }

    /** A node that draws a list as it is: a Picture's. */
    public static RenderNode ofList(DisplayList list) {
        RenderNode n = new RenderNode("picture");
        n.list = list;
        n.clipToBounds = false;
        return n;
    }

    public static RenderNode create(String name, Object animationHost) { return new RenderNode(name); }

    public RecordingCanvas beginRecording(int width, int height) {
        if (canvas != null && recording != null) throw new IllegalStateException("Recording currently in progress - missing #endRecording() call?");
        recording = new DisplayList();
        if (canvas == null) canvas = new RecordingCanvas(this, recording, width, height);
        else canvas.restart(recording, width, height);
        return canvas;
    }
    public RecordingCanvas beginRecording() { return beginRecording(getWidth(), getHeight()); }
    public void endRecording() {
        if (recording == null) throw new IllegalStateException("No recording in progress, forgot to call #beginRecording()?");
        recording.finish();
        list = recording;
        recording = null;
    }
    public boolean hasDisplayList() { return list != null; }
    public void discardDisplayList() { list = null; }
    public DisplayList displayList() { return list; }

    public boolean setPosition(int left, int top, int right, int bottom) {
        if (this.left == left && this.top == top && this.right == right && this.bottom == bottom) return false;
        this.left = left; this.top = top; this.right = right; this.bottom = bottom;
        return true;
    }
    public boolean setPosition(Rect r) { return setPosition(r.left, r.top, r.right, r.bottom); }
    public boolean setLeftTopRightBottom(int l, int t, int r, int b) { return setPosition(l, t, r, b); }
    public boolean offsetLeftAndRight(int offset) { if (offset == 0) return false; left += offset; right += offset; return true; }
    public boolean offsetTopAndBottom(int offset) { if (offset == 0) return false; top += offset; bottom += offset; return true; }
    public int getLeft() { return left; }
    public int getTop() { return top; }
    public int getRight() { return right; }
    public int getBottom() { return bottom; }
    public int getWidth() { return right - left; }
    public int getHeight() { return bottom - top; }
    public long getUniqueId() { return uniqueId; }
    public String getName() { return name; }

    public boolean setAlpha(float alpha) { this.alpha = alpha; return true; }
    public float getAlpha() { return alpha; }
    public boolean setTranslationX(float v) { translationX = v; return true; }
    public float getTranslationX() { return translationX; }
    public boolean setTranslationY(float v) { translationY = v; return true; }
    public float getTranslationY() { return translationY; }
    public boolean setTranslationZ(float v) { translationZ = v; return true; }
    public float getTranslationZ() { return translationZ; }
    public boolean setElevation(float v) { elevation = v; return true; }
    public float getElevation() { return elevation; }
    public boolean setScaleX(float v) { scaleX = v; return true; }
    public float getScaleX() { return scaleX; }
    public boolean setScaleY(float v) { scaleY = v; return true; }
    public float getScaleY() { return scaleY; }
    public boolean setRotationX(float v) { rotationX = v; return true; }
    public float getRotationX() { return rotationX; }
    public boolean setRotationY(float v) { rotationY = v; return true; }
    public float getRotationY() { return rotationY; }
    public boolean setRotationZ(float v) { rotationZ = v; return true; }
    public float getRotationZ() { return rotationZ; }
    public boolean setPivotX(float v) { pivotX = v; pivotSet = true; return true; }
    public float getPivotX() { return pivotSet ? pivotX : getWidth() / 2f; }
    public boolean setPivotY(float v) { pivotY = v; pivotSet = true; return true; }
    public float getPivotY() { return pivotSet ? pivotY : getHeight() / 2f; }
    public boolean isPivotExplicitlySet() { return pivotSet; }
    public boolean resetPivot() { pivotSet = false; return true; }
    public boolean setCameraDistance(float distance) { cameraDistance = distance; return true; }
    public float getCameraDistance() { return cameraDistance; }
    public boolean setClipToBounds(boolean clip) { clipToBounds = clip; return true; }
    public boolean getClipToBounds() { return clipToBounds; }
    public boolean setClipToOutline(boolean clip) { clipToOutline = clip; return true; }
    public boolean getClipToOutline() { return clipToOutline; }
    public boolean setClipRect(Rect rect) { hasClipRect = rect != null; if (rect != null) clipRect.set(rect); return true; }
    public boolean setOutline(Outline o) { if (o == null) outline.setEmpty(); else outline.set(o); return true; }
    public boolean hasShadow() { return !outline.isEmpty() && outline.getAlpha() != 0 && getZ() > 0; }
    public boolean setSpotShadowColor(int color) { spotShadow = color; return true; }
    public int getSpotShadowColor() { return spotShadow; }
    public boolean setAmbientShadowColor(int color) { ambientShadow = color; return true; }
    public int getAmbientShadowColor() { return ambientShadow; }
    public boolean setHasOverlappingRendering(boolean overlapping) { this.overlapping = overlapping; return true; }
    public boolean hasOverlappingRendering() { return overlapping; }
    public boolean setUseCompositingLayer(boolean forceToLayer, Paint paint) { compositing = forceToLayer; return true; }
    public boolean getUseCompositingLayer() { return compositing; }
    public boolean setRenderEffect(RenderEffect effect) { renderEffect = effect; return true; }
    public boolean setForceDarkAllowed(boolean allow) { forceDark = allow; return true; }
    public boolean isForceDarkAllowed() { return forceDark; }
    public boolean setProjectBackwards(boolean shouldProject) { return true; }
    public boolean setProjectionReceiver(boolean shouldReceive) { return true; }
    public boolean setAnimationMatrix(Matrix matrix) { animationMatrix = matrix != null ? new Matrix(matrix) : null; return true; }
    public Matrix getAnimationMatrix() { return animationMatrix; }
    public float getZ() { return elevation + translationZ; }
    public void addPositionUpdateListener(PositionUpdateListener listener) { positionListener = listener; }
    public void removePositionUpdateListener(PositionUpdateListener listener) { if (positionListener == listener) positionListener = null; }
    public boolean hasIdentityMatrix() {
        return translationX == 0 && translationY == 0 && scaleX == 1 && scaleY == 1 && rotationX == 0 && rotationY == 0 && rotationZ == 0;
    }
    public long computeApproximateMemoryUsage() { return 0; }
    public long computeApproximateMemoryAllocated() { return 0; }
    public boolean isForceDarkAllowedInternal() { return forceDark; }

    /** The node's transform without its position: translation, then rotation and scale about the pivot. */
    public void getMatrix(Matrix out) {
        out.reset();
        float px = getPivotX(), py = getPivotY();
        out.setTranslate(-px, -py);
        out.postScale(scaleX, scaleY);
        if (rotationZ != 0) out.postRotate(rotationZ);
        if (rotationX != 0 || rotationY != 0) {
            // a rotation about x or y seen head on: its shrink of the axes, without perspective
            out.postScale((float) Math.cos(Math.toRadians(rotationY)), (float) Math.cos(Math.toRadians(rotationX)));
        }
        out.postTranslate(px + translationX, py + translationY);
        if (animationMatrix != null) out.preConcat(animationMatrix);
    }

    public void getInverseMatrix(Matrix out) {
        Matrix m = new Matrix();
        getMatrix(m);
        if (!m.invert(out)) out.reset();
    }

    /** The node's transform in its parent: its position, then its own transform. */
    public void transformInParent(Matrix out) {
        getMatrix(out);
        out.postTranslate(left, top);
    }

    public Outline outline() { return outline; }
    public boolean hasClipRect() { return hasClipRect; }
    public Rect clipRect() { return clipRect; }

    @Override public String toString() { return "RenderNode{" + name + " " + left + "," + top + "-" + right + "," + bottom + "}"; }
}
