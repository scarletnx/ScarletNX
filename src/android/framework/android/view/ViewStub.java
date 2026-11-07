// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2008 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view;

import android.content.Context;
import android.graphics.Canvas;
import android.util.AttributeSet;
import java.lang.ref.WeakReference;

/** An invisible, empty view that a layout replaces it with when it becomes visible or is inflated. */
public final class ViewStub extends View {
    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private int layoutResource, inflatedId = NO_ID;
    private WeakReference<View> inflatedView;
    private LayoutInflater inflater;
    private OnInflateListener inflateListener;

    public interface OnInflateListener {
        void onInflate(ViewStub stub, View inflated);
    }

    public ViewStub(Context context) { this(context, 0); }
    public ViewStub(Context context, int layoutResource) { super(context); this.layoutResource = layoutResource; init(); }
    public ViewStub(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public ViewStub(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public ViewStub(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        if (attrs != null) {
            layoutResource = attrs.getAttributeResourceValue(ANDROID, "layout", 0);
            inflatedId = attrs.getAttributeResourceValue(ANDROID, "inflatedId", NO_ID);
            int id = attrs.getAttributeResourceValue(ANDROID, "id", NO_ID);
            if (id != NO_ID) setId(id);
        }
        init();
    }
    private void init() {
        setVisibility(GONE);
        setWillNotDraw(true);
    }

    public int getInflatedId() { return inflatedId; }
    public void setInflatedId(int inflatedId) { this.inflatedId = inflatedId; }
    public int getLayoutResource() { return layoutResource; }
    public void setLayoutResource(int layoutResource) { this.layoutResource = layoutResource; }
    public void setLayoutInflater(LayoutInflater inflater) { this.inflater = inflater; }
    public LayoutInflater getLayoutInflater() { return inflater; }
    public void setOnInflateListener(OnInflateListener inflateListener) { this.inflateListener = inflateListener; }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) { setMeasuredDimension(0, 0); }
    @Override public void draw(Canvas canvas) {}
    @Override protected void dispatchDraw(Canvas canvas) {}

    @Override public void setVisibility(int visibility) {
        View inflated = inflatedView != null ? inflatedView.get() : null;
        if (inflated != null) {
            inflated.setVisibility(visibility);
            return;
        }
        super.setVisibility(visibility);
        if (inflatedView == null && (visibility == VISIBLE || visibility == INVISIBLE) && getParent() instanceof ViewGroup) inflate();
    }

    /** Replaces the stub in its parent with the layout resource, and gives the inflated view. */
    public View inflate() {
        if (!(getParent() instanceof ViewGroup parent)) throw new IllegalStateException("ViewStub must have a non-null ViewGroup viewParent");
        if (layoutResource == 0) throw new IllegalArgumentException("ViewStub must have a valid layoutResource");
        LayoutInflater li = inflater != null ? inflater : LayoutInflater.from(getContext());
        View view = li.inflate(layoutResource, parent, false);
        if (inflatedId != NO_ID) view.setId(inflatedId);
        int index = parent.indexOfChild(this);
        parent.removeViewInLayout(this);
        ViewGroup.LayoutParams params = getLayoutParams();
        if (params != null) parent.addView(view, index, params);
        else parent.addView(view, index);
        inflatedView = new WeakReference<>(view);
        if (inflateListener != null) inflateListener.onInflate(this, view);
        return view;
    }
}
