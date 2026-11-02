// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.BlendMode;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.RenderEffect;
import android.graphics.RenderNode;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Parcelable;
import android.util.AttributeSet;
import android.util.SparseArray;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.autofill.AutofillId;
import android.view.autofill.AutofillValue;
import android.view.contentcapture.ContentCaptureSession;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import java.util.ArrayList;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * The base of the view tree: measure, layout, hardware drawing through its RenderNode, input dispatch, attach and
 * detach, insets. Accessibility, autofill and content capture report themselves off.
 */
public class View implements Drawable.Callback, KeyEvent.Callback {
    public static final int NO_ID = -1;
    public static final int VISIBLE = 0x0, INVISIBLE = 0x4, GONE = 0x8;
    public static final int MEASURED_SIZE_MASK = 0x00ffffff, MEASURED_STATE_MASK = 0xff000000,
        MEASURED_HEIGHT_STATE_SHIFT = 16, MEASURED_STATE_TOO_SMALL = 0x01000000;
    public static final int LAYER_TYPE_NONE = 0, LAYER_TYPE_SOFTWARE = 1, LAYER_TYPE_HARDWARE = 2;
    public static final int LAYOUT_DIRECTION_LTR = 0, LAYOUT_DIRECTION_RTL = 1, LAYOUT_DIRECTION_INHERIT = 2,
        LAYOUT_DIRECTION_LOCALE = 3;
    public static final int TEXT_DIRECTION_INHERIT = 0, TEXT_DIRECTION_FIRST_STRONG = 1, TEXT_DIRECTION_LTR = 3;
    public static final int TEXT_ALIGNMENT_INHERIT = 0, TEXT_ALIGNMENT_GRAVITY = 1;
    public static final int FOCUSABLE = 0x1, NOT_FOCUSABLE = 0x0, FOCUSABLE_AUTO = 0x10;
    public static final int FOCUS_BACKWARD = 0x01, FOCUS_FORWARD = 0x02, FOCUS_LEFT = 0x11, FOCUS_UP = 0x21,
        FOCUS_RIGHT = 0x42, FOCUS_DOWN = 0x82;
    public static final int IMPORTANT_FOR_ACCESSIBILITY_AUTO = 0, IMPORTANT_FOR_ACCESSIBILITY_YES = 1,
        IMPORTANT_FOR_ACCESSIBILITY_NO = 2, IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS = 4;
    public static final int IMPORTANT_FOR_AUTOFILL_AUTO = 0, IMPORTANT_FOR_AUTOFILL_YES = 1,
        IMPORTANT_FOR_AUTOFILL_NO = 2, IMPORTANT_FOR_AUTOFILL_YES_EXCLUDE_DESCENDANTS = 4,
        IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS = 8;
    public static final int IMPORTANT_FOR_CONTENT_CAPTURE_AUTO = 0, IMPORTANT_FOR_CONTENT_CAPTURE_YES = 1,
        IMPORTANT_FOR_CONTENT_CAPTURE_NO = 2, IMPORTANT_FOR_CONTENT_CAPTURE_YES_EXCLUDE_DESCENDANTS = 4,
        IMPORTANT_FOR_CONTENT_CAPTURE_NO_EXCLUDE_DESCENDANTS = 8;
    public static final int AUTOFILL_TYPE_NONE = 0, AUTOFILL_TYPE_TEXT = 1, AUTOFILL_TYPE_TOGGLE = 2,
        AUTOFILL_TYPE_LIST = 3, AUTOFILL_TYPE_DATE = 4;
    public static final int OVER_SCROLL_ALWAYS = 0, OVER_SCROLL_IF_CONTENT_SCROLLS = 1, OVER_SCROLL_NEVER = 2;
    public static final int SCROLL_AXIS_NONE = 0, SCROLL_AXIS_HORIZONTAL = 1, SCROLL_AXIS_VERTICAL = 2;
    public static final int SCROLL_INDICATOR_TOP = 0x1, SCROLL_INDICATOR_BOTTOM = 0x2;
    public static final int SYSTEM_UI_FLAG_VISIBLE = 0, SYSTEM_UI_FLAG_LOW_PROFILE = 0x1,
        SYSTEM_UI_FLAG_HIDE_NAVIGATION = 0x2, SYSTEM_UI_FLAG_FULLSCREEN = 0x4, SYSTEM_UI_FLAG_LAYOUT_STABLE = 0x100,
        SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION = 0x200, SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN = 0x400,
        SYSTEM_UI_FLAG_IMMERSIVE = 0x800, SYSTEM_UI_FLAG_IMMERSIVE_STICKY = 0x1000,
        SYSTEM_UI_FLAG_LIGHT_STATUS_BAR = 0x2000, SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR = 0x10;
    public static final int DRAG_FLAG_GLOBAL = 0x100;
    public static final int ACCESSIBILITY_LIVE_REGION_NONE = 0, ACCESSIBILITY_LIVE_REGION_POLITE = 1,
        ACCESSIBILITY_LIVE_REGION_ASSERTIVE = 2;
    public static final int HAPTIC_FEEDBACK_ENABLED = 0x10000000;
    public static final String AUTOFILL_HINT_PASSWORD = "password", AUTOFILL_HINT_USERNAME = "username",
        AUTOFILL_HINT_EMAIL_ADDRESS = "emailAddress";
    protected static final int[] EMPTY_STATE_SET = {};
    protected static final int[] ENABLED_STATE_SET = {0x0101009e};
    protected static final int[] PRESSED_ENABLED_STATE_SET = {0x010100a7, 0x0101009e};
    protected static final int[] FOCUSED_STATE_SET = {0x0101009c};

    public static final android.util.Property<View, Float> ALPHA = null, TRANSLATION_X = null, TRANSLATION_Y = null,
        SCALE_X = null, SCALE_Y = null;

    // ---- the tree ----
    protected Context mContext;
    ViewParent mParent;
    AttachInfo mAttachInfo;
    private int mId = NO_ID;
    private Object mTag;
    private SparseArray<Object> mKeyedTags;
    private ViewGroup.LayoutParams mLayoutParams;
    private int mVisibility = VISIBLE;
    private boolean mEnabled = true, mClickable, mLongClickable, mFocusable, mFocusableInTouchMode, mFocused,
        mPressed, mSelected, mActivated, mHovered, mWillNotDraw, mSaveEnabled = true, mSaveFromParentEnabled = true,
        mTransitionGroup, mNestedScrolling, mKeepScreenOn, mHapticEnabled = true, mSoundEnabled = true;

    // ---- geometry ----
    protected int mLeft, mTop, mRight, mBottom, mScrollX, mScrollY;
    protected int mPaddingLeft, mPaddingTop, mPaddingRight, mPaddingBottom;
    private int mMeasuredWidth, mMeasuredHeight, mMinWidth, mMinHeight;
    private int mOldWidthSpec = Integer.MIN_VALUE, mOldHeightSpec = Integer.MIN_VALUE;
    private boolean mLayoutRequested = true, mForceLayout = true, mMeasuredSet, mLaidOut;
    final RenderNode mRenderNode;
    private boolean mDisplayListDirty = true;
    private ViewOutlineProvider mOutlineProvider = ViewOutlineProvider.BACKGROUND;
    private Rect mClipBounds;

    // ---- drawing ----
    private Drawable mBackground, mForeground;
    private ColorStateList mBackgroundTint;
    private PorterDuff.Mode mBackgroundTintMode;
    private int mLayerType = LAYER_TYPE_NONE;
    private int[] mDrawableState;

    // ---- listeners ----
    private OnClickListener mOnClickListener;
    private OnLongClickListener mOnLongClickListener;
    private OnTouchListener mOnTouchListener;
    private OnKeyListener mOnKeyListener;
    private OnFocusChangeListener mOnFocusChangeListener;
    private OnGenericMotionListener mOnGenericMotionListener;
    private OnHoverListener mOnHoverListener;
    private OnDragListener mOnDragListener;
    private OnApplyWindowInsetsListener mOnApplyWindowInsetsListener;
    private OnSystemUiVisibilityChangeListener mOnSystemUiVisibilityChangeListener;
    private OnScrollChangeListener mOnScrollChangeListener;
    private final CopyOnWriteArrayList<OnAttachStateChangeListener> mAttachListeners = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnLayoutChangeListener> mLayoutListeners = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnUnhandledKeyEventListener> mUnhandledKeyListeners = new CopyOnWriteArrayList<>();
    private AccessibilityDelegate mAccessibilityDelegate;
    private final ArrayList<Runnable> mPendingRunnables = new ArrayList<>();
    private ArrayList<long[]> mPendingDelays;

    // ---- the rest ----
    private CharSequence mContentDescription, mStateDescription, mAccessibilityPaneTitle, mTooltipText;
    private String mTransitionName;
    private int mImportantForAccessibility, mImportantForAutofill, mImportantForContentCapture;
    private int mLayoutDirection = LAYOUT_DIRECTION_INHERIT, mSystemUiVisibility, mScrollIndicators;
    private int mOverScrollMode = OVER_SCROLL_IF_CONTENT_SCROLLS;
    private AutofillId mAutofillId;
    private PointerIcon mPointerIcon;
    private boolean mAccessibilityHeading, mScreenReaderFocusable, mDefaultFocusHighlight = true;
    private static int sNextGeneratedId = 1;
    private static long sNextDrawingId = 1;
    private final long mUniqueDrawingId;

    public View(Context context) {
        mContext = context;
        mRenderNode = new RenderNode(getClass().getName());
        synchronized (View.class) { mUniqueDrawingId = sNextDrawingId++; }
    }

    public View(Context context, AttributeSet attrs) { this(context); }
    public View(Context context, AttributeSet attrs, int defStyleAttr) { this(context); }
    public View(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { this(context); }

    // ---- listener interfaces ----
    public interface OnClickListener { void onClick(View v); }
    public interface OnLongClickListener { boolean onLongClick(View v); default boolean onLongClickUseDefaultHapticFeedback(View v) { return true; } }
    public interface OnTouchListener { boolean onTouch(View v, MotionEvent event); }
    public interface OnKeyListener { boolean onKey(View v, int keyCode, KeyEvent event); }
    public interface OnFocusChangeListener { void onFocusChange(View v, boolean hasFocus); }
    public interface OnGenericMotionListener { boolean onGenericMotion(View v, MotionEvent event); }
    public interface OnHoverListener { boolean onHover(View v, MotionEvent event); }
    public interface OnDragListener { boolean onDrag(View v, DragEvent event); }
    public interface OnApplyWindowInsetsListener { WindowInsets onApplyWindowInsets(View v, WindowInsets insets); }
    public interface OnSystemUiVisibilityChangeListener { void onSystemUiVisibilityChange(int visibility); }
    public interface OnScrollChangeListener { void onScrollChange(View v, int scrollX, int scrollY, int oldScrollX, int oldScrollY); }
    public interface OnCreateContextMenuListener { void onCreateContextMenu(ContextMenu menu, View v, ContextMenu.ContextMenuInfo menuInfo); }
    public interface OnUnhandledKeyEventListener { boolean onUnhandledKeyEvent(View v, KeyEvent event); }
    public interface OnCapturedPointerListener { boolean onCapturedPointer(View view, MotionEvent event); }
    public interface OnContextClickListener { boolean onContextClick(View v); }
    public interface OnAttachStateChangeListener {
        void onViewAttachedToWindow(View v);

        void onViewDetachedFromWindow(View v);
    }
    public interface OnLayoutChangeListener {
        void onLayoutChange(View v, int left, int top, int right, int bottom, int oldLeft, int oldTop, int oldRight, int oldBottom);
    }

    /** What views of one window share: its root, its handler, its tree observer. */
    public static final class AttachInfo {
        public final ViewRootImpl root;
        public final Handler handler;
        public final ViewTreeObserver treeObserver;
        public final IBinder windowToken;
        public View rootView;
        public boolean hasWindowFocus;
        public int windowVisibility = VISIBLE;
        public long drawingTime;

        public AttachInfo(ViewRootImpl root, Handler handler, IBinder token) {
            this.root = root;
            this.handler = handler;
            windowToken = token;
            treeObserver = new ViewTreeObserver();
        }
    }

    public static class MeasureSpec {
        public static final int MODE_SHIFT = 30, MODE_MASK = 0x3 << MODE_SHIFT, UNSPECIFIED = 0,
            EXACTLY = 1 << MODE_SHIFT, AT_MOST = 2 << MODE_SHIFT;

        public static int makeMeasureSpec(int size, int mode) { return (size & ~MODE_MASK) | (mode & MODE_MASK); }
        public static int makeSafeMeasureSpec(int size, int mode) { return makeMeasureSpec(size, mode); }
        public static int getMode(int measureSpec) { return measureSpec & MODE_MASK; }
        public static int getSize(int measureSpec) { return measureSpec & ~MODE_MASK; }
        public static String toString(int spec) {
            int mode = getMode(spec);
            return "MeasureSpec: " + (mode == EXACTLY ? "EXACTLY " : mode == AT_MOST ? "AT_MOST " : "UNSPECIFIED ") + getSize(spec);
        }
    }

    public static class AccessibilityDelegate {
        public AccessibilityDelegate() {}
        public void sendAccessibilityEvent(View host, int eventType) { host.sendAccessibilityEventInternal(eventType); }
        public boolean performAccessibilityAction(View host, int action, Bundle args) { return host.performAccessibilityActionInternal(action, args); }
        public void sendAccessibilityEventUnchecked(View host, AccessibilityEvent event) {}
        public boolean dispatchPopulateAccessibilityEvent(View host, AccessibilityEvent event) { return false; }
        public void onPopulateAccessibilityEvent(View host, AccessibilityEvent event) {}
        public void onInitializeAccessibilityEvent(View host, AccessibilityEvent event) {}
        public void onInitializeAccessibilityNodeInfo(View host, AccessibilityNodeInfo info) {}
        public void addExtraDataToAccessibilityNodeInfo(View host, AccessibilityNodeInfo info, String extraDataKey, Bundle arguments) {}
        public boolean onRequestSendAccessibilityEvent(ViewGroup host, View child, AccessibilityEvent event) { return true; }
        public AccessibilityNodeProvider getAccessibilityNodeProvider(View host) { return null; }
    }

    public static class BaseSavedState extends android.view.AbsSavedState {
        public BaseSavedState(Parcelable superState) { super(superState); }
        public BaseSavedState(android.os.Parcel source) { super(source); }
    }

    // ---- identity and tags ----
    public static int generateViewId() { synchronized (View.class) { return sNextGeneratedId++; } }
    public int getId() { return mId; }
    public void setId(int id) { mId = id; }
    public Object getTag() { return mTag; }
    public void setTag(Object tag) { mTag = tag; }
    public Object getTag(int key) { return mKeyedTags != null ? mKeyedTags.get(key) : null; }
    public void setTag(int key, Object tag) {
        if ((key >>> 24) < 2) throw new IllegalArgumentException("The key must be an application-specific resource id.");
        if (mKeyedTags == null) mKeyedTags = new SparseArray<>(2);
        mKeyedTags.put(key, tag);
    }
    public long getUniqueDrawingId() { return mUniqueDrawingId; }
    public final Context getContext() { return mContext; }
    public Resources getResources() { return mContext != null ? mContext.getResources() : null; }
    public final ViewParent getParent() { return mParent; }
    public ViewParent getParentForAccessibility() { return mParent; }
    public View getRootView() {
        if (mAttachInfo != null && mAttachInfo.rootView != null) return mAttachInfo.rootView;
        View v = this;
        while (v.mParent instanceof View p) v = p;
        return v;
    }
    public final <T extends View> T findViewById(int id) { return id == NO_ID ? null : findViewTraversal(id); }
    public final <T extends View> T requireViewById(int id) {
        T v = findViewById(id);
        if (v == null) throw new IllegalArgumentException("ID does not reference a View inside this View");
        return v;
    }
    @SuppressWarnings("unchecked")
    protected <T extends View> T findViewTraversal(int id) { return id == mId ? (T) this : null; }
    public final <T extends View> T findViewWithTag(Object tag) { return tag == null ? null : findViewWithTagTraversal(tag); }
    @SuppressWarnings("unchecked")
    protected <T extends View> T findViewWithTagTraversal(Object tag) { return tag.equals(mTag) ? (T) this : null; }
    public boolean isInEditMode() { return false; }
    public boolean isHardwareAccelerated() { return true; }
    public void setLayerType(int layerType, Paint paint) { mLayerType = layerType; mRenderNode.setUseCompositingLayer(layerType == LAYER_TYPE_HARDWARE, paint); invalidate(); }
    public int getLayerType() { return mLayerType; }
    public void setLayerPaint(Paint paint) {}

    // ---- attach ----
    public boolean isAttachedToWindow() { return mAttachInfo != null; }
    public Handler getHandler() { return mAttachInfo != null ? mAttachInfo.handler : null; }
    public ViewTreeObserver getViewTreeObserver() {
        if (mAttachInfo != null) return mAttachInfo.treeObserver;
        if (mFloatingTreeObserver == null) mFloatingTreeObserver = new ViewTreeObserver();
        return mFloatingTreeObserver;
    }
    private ViewTreeObserver mFloatingTreeObserver;
    public IBinder getWindowToken() { return mAttachInfo != null ? mAttachInfo.windowToken : null; }
    public IBinder getApplicationWindowToken() { return getWindowToken(); }
    public Display getDisplay() { return mAttachInfo != null ? Display.DEFAULT : null; }
    public ViewRootImpl getViewRootImpl() { return mAttachInfo != null ? mAttachInfo.root : null; }
    public int getWindowVisibility() { return mAttachInfo != null ? mAttachInfo.windowVisibility : GONE; }
    public boolean hasWindowFocus() { return mAttachInfo != null && mAttachInfo.hasWindowFocus; }
    public void addOnAttachStateChangeListener(OnAttachStateChangeListener l) { mAttachListeners.add(l); }
    public void removeOnAttachStateChangeListener(OnAttachStateChangeListener l) { mAttachListeners.remove(l); }

    void dispatchAttachedToWindow(AttachInfo info, int visibility) {
        mAttachInfo = info;
        if (mFloatingTreeObserver != null) { info.treeObserver.merge(mFloatingTreeObserver); mFloatingTreeObserver = null; }
        ArrayList<Runnable> pending = new ArrayList<>(mPendingRunnables);
        ArrayList<long[]> delays = mPendingDelays;
        mPendingRunnables.clear();
        mPendingDelays = null;
        for (int i = 0; i < pending.size(); ++i) {
            long delay = delays != null && i < delays.size() ? delays.get(i)[0] : 0;
            info.handler.postDelayed(pending.get(i), delay);
        }
        onAttachedToWindow();
        for (OnAttachStateChangeListener l : mAttachListeners) l.onViewAttachedToWindow(this);
        onWindowVisibilityChanged(info.windowVisibility);
        if (info.hasWindowFocus) onWindowFocusChanged(true);
        refreshDrawableState();
        if (mLayoutRequested) requestLayout();
        invalidate();
    }

    void dispatchDetachedFromWindow() {
        onDetachedFromWindow();
        for (OnAttachStateChangeListener l : mAttachListeners) l.onViewDetachedFromWindow(this);
        mAttachInfo = null;
    }

    protected void onAttachedToWindow() {}
    protected void onDetachedFromWindow() {}
    protected void onWindowVisibilityChanged(int visibility) {}
    public void onWindowFocusChanged(boolean hasWindowFocus) {}
    public void dispatchWindowFocusChanged(boolean hasFocus) { onWindowFocusChanged(hasFocus); }
    public void dispatchWindowVisibilityChanged(int visibility) { onWindowVisibilityChanged(visibility); }
    protected void onConfigurationChanged(Configuration newConfig) {}
    public void dispatchConfigurationChanged(Configuration newConfig) { onConfigurationChanged(newConfig); }
    protected void onVisibilityChanged(View changedView, int visibility) {}
    void dispatchVisibilityChanged(View changedView, int visibility) { onVisibilityChanged(changedView, visibility); }

    // ---- posting ----
    public boolean post(Runnable action) {
        if (mAttachInfo != null) return mAttachInfo.handler.post(action);
        mPendingRunnables.add(action);
        if (mPendingDelays != null) mPendingDelays.add(new long[] {0});
        return true;
    }
    public boolean postDelayed(Runnable action, long delayMillis) {
        if (mAttachInfo != null) return mAttachInfo.handler.postDelayed(action, delayMillis);
        if (mPendingDelays == null) {
            mPendingDelays = new ArrayList<>();
            for (int i = 0; i < mPendingRunnables.size(); ++i) mPendingDelays.add(new long[] {0});
        }
        mPendingRunnables.add(action);
        mPendingDelays.add(new long[] {delayMillis});
        return true;
    }
    public void postOnAnimation(Runnable action) {
        if (mAttachInfo != null) Choreographer.getInstance().postCallback(Choreographer.CALLBACK_ANIMATION, action, null);
        else post(action);
    }
    public void postOnAnimationDelayed(Runnable action, long delayMillis) {
        if (mAttachInfo != null) Choreographer.getInstance().postCallbackDelayed(Choreographer.CALLBACK_ANIMATION, action, null, delayMillis);
        else postDelayed(action, delayMillis);
    }
    public boolean removeCallbacks(Runnable action) {
        if (action == null) return true;
        if (mAttachInfo != null) {
            mAttachInfo.handler.removeCallbacks(action);
            Choreographer.getInstance().removeCallbacks(Choreographer.CALLBACK_ANIMATION, action, null);
        }
        int i = mPendingRunnables.indexOf(action);
        while (i >= 0) {
            mPendingRunnables.remove(i);
            if (mPendingDelays != null) mPendingDelays.remove(i);
            i = mPendingRunnables.indexOf(action);
        }
        return true;
    }
    public void postInvalidate() { post(this::invalidate); }
    public void postInvalidateDelayed(long delayMilliseconds) { postDelayed(this::invalidate, delayMilliseconds); }
    public void postInvalidateOnAnimation() { postOnAnimation(this::invalidate); }
    public void cancelPendingInputEvents() {}

    // ---- visibility and state ----
    public int getVisibility() { return mVisibility; }
    public void setVisibility(int visibility) {
        if (mVisibility == visibility) return;
        boolean layout = mVisibility == GONE || visibility == GONE;
        mVisibility = visibility;
        if (layout) requestLayout();
        invalidateSelfAndParent();
        if (mParent instanceof ViewGroup g) g.onChildVisibilityChanged(this, visibility);
        dispatchVisibilityChanged(this, visibility);
    }
    public boolean isShown() {
        View v = this;
        while (v != null) {
            if (v.mVisibility != VISIBLE) return false;
            if (!(v.mParent instanceof View p)) return v.mAttachInfo != null;
            v = p;
        }
        return false;
    }
    public boolean isEnabled() { return mEnabled; }
    public void setEnabled(boolean enabled) { if (mEnabled != enabled) { mEnabled = enabled; refreshDrawableState(); invalidate(); } }
    public boolean isClickable() { return mClickable; }
    public void setClickable(boolean clickable) { mClickable = clickable; }
    public boolean isLongClickable() { return mLongClickable; }
    public void setLongClickable(boolean longClickable) { mLongClickable = longClickable; }
    public boolean isContextClickable() { return false; }
    public boolean isPressed() { return mPressed; }
    public void setPressed(boolean pressed) { if (mPressed != pressed) { mPressed = pressed; refreshDrawableState(); dispatchSetPressed(pressed); } }
    protected void dispatchSetPressed(boolean pressed) {}
    public boolean isSelected() { return mSelected; }
    public void setSelected(boolean selected) { if (mSelected != selected) { mSelected = selected; refreshDrawableState(); invalidate(); } }
    public boolean isActivated() { return mActivated; }
    public void setActivated(boolean activated) { if (mActivated != activated) { mActivated = activated; refreshDrawableState(); invalidate(); } }
    public boolean isHovered() { return mHovered; }
    public void setHovered(boolean hovered) { if (mHovered != hovered) { mHovered = hovered; refreshDrawableState(); onHoverChanged(hovered); } }
    public void onHoverChanged(boolean hovered) {}
    public boolean getKeepScreenOn() { return mKeepScreenOn; }
    public void setKeepScreenOn(boolean keepScreenOn) { mKeepScreenOn = keepScreenOn; }
    public void setWillNotDraw(boolean willNotDraw) { mWillNotDraw = willNotDraw; }
    public boolean willNotDraw() { return mWillNotDraw; }
    public void setSaveEnabled(boolean enabled) { mSaveEnabled = enabled; }
    public boolean isSaveEnabled() { return mSaveEnabled; }
    public void setSaveFromParentEnabled(boolean enabled) { mSaveFromParentEnabled = enabled; }
    public boolean isSaveFromParentEnabled() { return mSaveFromParentEnabled; }
    public boolean isHapticFeedbackEnabled() { return mHapticEnabled; }
    public void setHapticFeedbackEnabled(boolean enabled) { mHapticEnabled = enabled; }
    public boolean performHapticFeedback(int feedbackConstant) { return mHapticEnabled; }
    public boolean performHapticFeedback(int feedbackConstant, int flags) { return mHapticEnabled; }
    public void setSoundEffectsEnabled(boolean enabled) { mSoundEnabled = enabled; }
    public boolean isSoundEffectsEnabled() { return mSoundEnabled; }
    public void playSoundEffect(int soundConstant) {}
    public int getLayoutDirection() {
        if (mLayoutDirection == LAYOUT_DIRECTION_RTL) return LAYOUT_DIRECTION_RTL;
        if (mLayoutDirection == LAYOUT_DIRECTION_INHERIT && mParent instanceof View p) return p.getLayoutDirection();
        return LAYOUT_DIRECTION_LTR;
    }
    public int getRawLayoutDirection() { return mLayoutDirection; }
    public void setLayoutDirection(int layoutDirection) { if (mLayoutDirection != layoutDirection) { mLayoutDirection = layoutDirection; requestLayout(); } }
    public boolean isLayoutDirectionResolved() { return true; }
    public int getTextDirection() { return TEXT_DIRECTION_FIRST_STRONG; }
    public void setTextDirection(int textDirection) {}
    public int getTextAlignment() { return TEXT_ALIGNMENT_GRAVITY; }
    public void setTextAlignment(int textAlignment) {}
    public int getOverScrollMode() { return mOverScrollMode; }
    public void setOverScrollMode(int overScrollMode) { mOverScrollMode = overScrollMode; }

    // ---- drawable state ----
    public final int[] getDrawableState() {
        if (mDrawableState == null) mDrawableState = onCreateDrawableState(0);
        return mDrawableState;
    }
    protected int[] onCreateDrawableState(int extraSpace) {
        ArrayList<Integer> s = new ArrayList<>();
        if (mEnabled) s.add(0x0101009e);
        if (mPressed) s.add(0x010100a7);
        if (mFocused) s.add(0x0101009c);
        if (mSelected) s.add(0x010100a1);
        if (mActivated) s.add(0x010102fe);
        if (mHovered) s.add(0x01010367);
        if (hasWindowFocus()) s.add(0x0101009d);
        int[] out = new int[s.size() + extraSpace];
        for (int i = 0; i < s.size(); ++i) out[i] = s.get(i);
        return out;
    }
    protected static int[] mergeDrawableStates(int[] baseState, int[] additionalState) {
        int i = baseState.length - 1;
        while (i >= 0 && baseState[i] == 0) --i;
        System.arraycopy(additionalState, 0, baseState, i + 1, additionalState.length);
        return baseState;
    }
    public void refreshDrawableState() {
        mDrawableState = null;
        drawableStateChanged();
        if (mParent instanceof ViewGroup g) g.childDrawableStateChanged(this);
    }
    protected void drawableStateChanged() {
        int[] state = getDrawableState();
        boolean changed = false;
        if (mBackground != null && mBackground.isStateful()) changed |= mBackground.setState(state);
        if (mForeground != null && mForeground.isStateful()) changed |= mForeground.setState(state);
        if (changed) invalidate();
    }
    public void jumpDrawablesToCurrentState() {
        if (mBackground != null) mBackground.jumpToCurrentState();
        if (mForeground != null) mForeground.jumpToCurrentState();
    }
    public void drawableHotspotChanged(float x, float y) {}
    public void dispatchDrawableHotspotChanged(float x, float y) {}

    // ---- background ----
    public Drawable getBackground() { return mBackground; }
    public void setBackground(Drawable background) {
        if (mBackground == background) return;
        if (mBackground != null) mBackground.setCallback(null);
        mBackground = background;
        if (background != null) {
            background.setCallback(this);
            if (background.isStateful()) background.setState(getDrawableState());
            if (mBackgroundTint != null) background.setTintList(mBackgroundTint);
        }
        requestLayout();
        invalidate();
    }
    @Deprecated public void setBackgroundDrawable(Drawable background) { setBackground(background); }
    public void setBackgroundColor(int color) {
        if (mBackground instanceof android.graphics.drawable.ColorDrawable c) { c.setColor(color); invalidate(); }
        else setBackground(new android.graphics.drawable.ColorDrawable(color));
    }
    public void setBackgroundResource(int resid) { setBackground(resid != 0 ? mContext.getDrawable(resid) : null); }
    public void setBackgroundTintList(ColorStateList tint) { mBackgroundTint = tint; if (mBackground != null) mBackground.setTintList(tint); }
    public ColorStateList getBackgroundTintList() { return mBackgroundTint; }
    public void setBackgroundTintMode(PorterDuff.Mode tintMode) { mBackgroundTintMode = tintMode; }
    public PorterDuff.Mode getBackgroundTintMode() { return mBackgroundTintMode; }
    public void setBackgroundTintBlendMode(BlendMode blendMode) {}
    public Drawable getForeground() { return mForeground; }
    public void setForeground(Drawable foreground) {
        if (mForeground != null) mForeground.setCallback(null);
        mForeground = foreground;
        if (foreground != null) foreground.setCallback(this);
        invalidate();
    }
    @Override public void invalidateDrawable(Drawable drawable) { if (drawable == mBackground || drawable == mForeground) invalidate(); }
    @Override public void scheduleDrawable(Drawable who, Runnable what, long when) { postDelayed(what, when - android.os.SystemClock.uptimeMillis()); }
    @Override public void unscheduleDrawable(Drawable who, Runnable what) { removeCallbacks(what); }
    public void unscheduleDrawable(Drawable who) {}
    protected boolean verifyDrawable(Drawable who) { return who == mBackground || who == mForeground; }

    // ---- padding ----
    public void setPadding(int left, int top, int right, int bottom) {
        if (mPaddingLeft != left || mPaddingTop != top || mPaddingRight != right || mPaddingBottom != bottom) {
            mPaddingLeft = left; mPaddingTop = top; mPaddingRight = right; mPaddingBottom = bottom;
            requestLayout();
            invalidate();
        }
    }
    public void setPaddingRelative(int start, int top, int end, int bottom) { setPadding(start, top, end, bottom); }
    public int getPaddingLeft() { return mPaddingLeft; }
    public int getPaddingTop() { return mPaddingTop; }
    public int getPaddingRight() { return mPaddingRight; }
    public int getPaddingBottom() { return mPaddingBottom; }
    public int getPaddingStart() { return mPaddingLeft; }
    public int getPaddingEnd() { return mPaddingRight; }
    public boolean isPaddingRelative() { return false; }

    // ---- geometry ----
    public final int getLeft() { return mLeft; }
    public final int getTop() { return mTop; }
    public final int getRight() { return mRight; }
    public final int getBottom() { return mBottom; }
    public final int getWidth() { return mRight - mLeft; }
    public final int getHeight() { return mBottom - mTop; }
    public final void setLeft(int left) { if (left != mLeft) { mLeft = left; positionChanged(); } }
    public final void setTop(int top) { if (top != mTop) { mTop = top; positionChanged(); } }
    public final void setRight(int right) { if (right != mRight) { mRight = right; positionChanged(); } }
    public final void setBottom(int bottom) { if (bottom != mBottom) { mBottom = bottom; positionChanged(); } }
    public void setLeftTopRightBottom(int l, int t, int r, int b) { setFrame(l, t, r, b); }
    private void positionChanged() { mRenderNode.setPosition(mLeft, mTop, mRight, mBottom); invalidateSelfAndParent(); }
    public void offsetLeftAndRight(int offset) { if (offset != 0) { mLeft += offset; mRight += offset; mRenderNode.offsetLeftAndRight(offset); invalidateSelfAndParent(); } }
    public void offsetTopAndBottom(int offset) { if (offset != 0) { mTop += offset; mBottom += offset; mRenderNode.offsetTopAndBottom(offset); invalidateSelfAndParent(); } }
    public final int getScrollX() { return mScrollX; }
    public final int getScrollY() { return mScrollY; }
    public void scrollTo(int x, int y) {
        if (mScrollX != x || mScrollY != y) {
            int ox = mScrollX, oy = mScrollY;
            mScrollX = x; mScrollY = y;
            onScrollChanged(x, y, ox, oy);
            if (mOnScrollChangeListener != null) mOnScrollChangeListener.onScrollChange(this, x, y, ox, oy);
            invalidate();
        }
    }
    public void scrollBy(int x, int y) { scrollTo(mScrollX + x, mScrollY + y); }
    protected void onScrollChanged(int l, int t, int oldl, int oldt) {}
    public void setScrollX(int value) { scrollTo(value, mScrollY); }
    public void setScrollY(int value) { scrollTo(mScrollX, value); }
    public void setOnScrollChangeListener(OnScrollChangeListener l) { mOnScrollChangeListener = l; }
    public boolean canScrollHorizontally(int direction) { return false; }
    public boolean canScrollVertically(int direction) { return false; }
    public void setVerticalScrollBarEnabled(boolean enabled) {}
    public void setHorizontalScrollBarEnabled(boolean enabled) {}
    public void setScrollIndicators(int indicators) { mScrollIndicators = indicators; }
    public void setScrollIndicators(int indicators, int mask) { mScrollIndicators = (mScrollIndicators & ~mask) | (indicators & mask); }
    public int getScrollIndicators() { return mScrollIndicators; }
    public void getDrawingRect(Rect outRect) { outRect.set(mScrollX, mScrollY, mScrollX + getWidth(), mScrollY + getHeight()); }
    public void getHitRect(Rect outRect) {
        RectF r = new RectF(0, 0, getWidth(), getHeight());
        getMatrix().mapRect(r);
        r.offset(mLeft, mTop);
        r.roundOut(outRect);
    }
    public void getFocusedRect(Rect r) { getDrawingRect(r); }
    public boolean getLocalVisibleRect(Rect r) { r.set(0, 0, getWidth(), getHeight()); return isShown(); }
    public boolean getGlobalVisibleRect(Rect r) { return getGlobalVisibleRect(r, null); }
    public boolean getGlobalVisibleRect(Rect r, android.graphics.Point globalOffset) {
        int[] loc = new int[2];
        getLocationInWindow(loc);
        r.set(loc[0], loc[1], loc[0] + getWidth(), loc[1] + getHeight());
        if (globalOffset != null) globalOffset.set(loc[0], loc[1]);
        return isShown() && !r.isEmpty();
    }
    public void getWindowVisibleDisplayFrame(Rect outRect) {
        android.util.DisplayMetrics m = snx.android.Device.metrics();
        outRect.set(0, 0, m.widthPixels, m.heightPixels);
    }
    public void getWindowDisplayFrame(Rect outRect) { getWindowVisibleDisplayFrame(outRect); }
    public void setClipBounds(Rect clipBounds) { mClipBounds = clipBounds != null ? new Rect(clipBounds) : null; mRenderNode.setClipRect(mClipBounds); invalidate(); }
    public Rect getClipBounds() { return mClipBounds != null ? new Rect(mClipBounds) : null; }
    public boolean getClipBounds(Rect outRect) { if (mClipBounds == null) return false; outRect.set(mClipBounds); return true; }

    /** The point's place in the window: up the parents, with each one's transform and scroll. */
    public void getLocationInWindow(int[] outLocation) {
        float[] p = {0, 0};
        transformToWindow(p);
        outLocation[0] = Math.round(p[0]);
        outLocation[1] = Math.round(p[1]);
    }
    private void transformToWindow(float[] p) {
        View v = this;
        while (v != null) {
            v.getMatrix().mapPoints(p);
            p[0] += v.mLeft;
            p[1] += v.mTop;
            if (!(v.mParent instanceof View parent)) break;
            p[0] -= parent.mScrollX;
            p[1] -= parent.mScrollY;
            v = parent;
        }
    }
    public void getLocationOnScreen(int[] outLocation) {
        getLocationInWindow(outLocation);
        if (mAttachInfo != null) {
            outLocation[0] += mAttachInfo.root.windowX();
            outLocation[1] += mAttachInfo.root.windowY();
        }
    }
    public void getLocationInSurface(int[] location) { getLocationInWindow(location); }
    public void transformMatrixToGlobal(Matrix matrix) {
        View v = this;
        Matrix m = new Matrix();
        while (v != null) {
            matrix.postConcat(v.getMatrix());
            matrix.postTranslate(v.mLeft, v.mTop);
            if (!(v.mParent instanceof View parent)) break;
            matrix.postTranslate(-parent.mScrollX, -parent.mScrollY);
            v = parent;
        }
        m.reset();
    }
    public void transformMatrixToLocal(Matrix matrix) {
        Matrix g = new Matrix();
        transformMatrixToGlobal(g);
        Matrix inverse = new Matrix();
        g.invert(inverse);
        matrix.postConcat(inverse);
    }
    public boolean requestRectangleOnScreen(Rect rectangle) { return false; }
    public boolean requestRectangleOnScreen(Rect rectangle, boolean immediate) { return false; }

    // ---- transform properties (the RenderNode's) ----
    public Matrix getMatrix() { Matrix m = new Matrix(); mRenderNode.getMatrix(m); return m; }
    public boolean hasIdentityMatrix() { return mRenderNode.hasIdentityMatrix(); }
    public float getAlpha() { return mRenderNode.getAlpha(); }
    public void setAlpha(float alpha) { if (mRenderNode.getAlpha() != alpha) { mRenderNode.setAlpha(alpha); invalidateViewProperty(); } }
    public float getTranslationX() { return mRenderNode.getTranslationX(); }
    public void setTranslationX(float v) { mRenderNode.setTranslationX(v); invalidateViewProperty(); }
    public float getTranslationY() { return mRenderNode.getTranslationY(); }
    public void setTranslationY(float v) { mRenderNode.setTranslationY(v); invalidateViewProperty(); }
    public float getTranslationZ() { return mRenderNode.getTranslationZ(); }
    public void setTranslationZ(float v) { mRenderNode.setTranslationZ(v); invalidateViewProperty(); }
    public float getElevation() { return mRenderNode.getElevation(); }
    public void setElevation(float v) { mRenderNode.setElevation(v); invalidateViewProperty(); }
    public float getZ() { return getElevation() + getTranslationZ(); }
    public void setZ(float z) { setTranslationZ(z - getElevation()); }
    public float getX() { return mLeft + getTranslationX(); }
    public void setX(float x) { setTranslationX(x - mLeft); }
    public float getY() { return mTop + getTranslationY(); }
    public void setY(float y) { setTranslationY(y - mTop); }
    public float getScaleX() { return mRenderNode.getScaleX(); }
    public void setScaleX(float v) { mRenderNode.setScaleX(v); invalidateViewProperty(); }
    public float getScaleY() { return mRenderNode.getScaleY(); }
    public void setScaleY(float v) { mRenderNode.setScaleY(v); invalidateViewProperty(); }
    public float getRotation() { return mRenderNode.getRotationZ(); }
    public void setRotation(float v) { mRenderNode.setRotationZ(v); invalidateViewProperty(); }
    public float getRotationX() { return mRenderNode.getRotationX(); }
    public void setRotationX(float v) { mRenderNode.setRotationX(v); invalidateViewProperty(); }
    public float getRotationY() { return mRenderNode.getRotationY(); }
    public void setRotationY(float v) { mRenderNode.setRotationY(v); invalidateViewProperty(); }
    public float getPivotX() { return mRenderNode.getPivotX(); }
    public void setPivotX(float v) { mRenderNode.setPivotX(v); invalidateViewProperty(); }
    public float getPivotY() { return mRenderNode.getPivotY(); }
    public void setPivotY(float v) { mRenderNode.setPivotY(v); invalidateViewProperty(); }
    public boolean isPivotSet() { return mRenderNode.isPivotExplicitlySet(); }
    public void resetPivot() { mRenderNode.resetPivot(); invalidateViewProperty(); }
    public float getCameraDistance() { return mRenderNode.getCameraDistance(); }
    public void setCameraDistance(float distance) { mRenderNode.setCameraDistance(distance); }
    public void setOutlineProvider(ViewOutlineProvider provider) { mOutlineProvider = provider; invalidateOutline(); }
    public ViewOutlineProvider getOutlineProvider() { return mOutlineProvider; }
    public void invalidateOutline() {
        Outline o = new Outline();
        o.setAlpha(1);
        if (mOutlineProvider != null) mOutlineProvider.getOutline(this, o);
        mRenderNode.setOutline(o);
        invalidateViewProperty();
    }
    public final boolean getClipToOutline() { return mRenderNode.getClipToOutline(); }
    public void setClipToOutline(boolean clipToOutline) { mRenderNode.setClipToOutline(clipToOutline); invalidateViewProperty(); }
    public void setOutlineAmbientShadowColor(int color) { mRenderNode.setAmbientShadowColor(color); }
    public int getOutlineAmbientShadowColor() { return mRenderNode.getAmbientShadowColor(); }
    public void setOutlineSpotShadowColor(int color) { mRenderNode.setSpotShadowColor(color); }
    public int getOutlineSpotShadowColor() { return mRenderNode.getSpotShadowColor(); }
    public void setRenderEffect(RenderEffect renderEffect) { mRenderNode.setRenderEffect(renderEffect); invalidateViewProperty(); }
    public void setForceDarkAllowed(boolean allow) { mRenderNode.setForceDarkAllowed(allow); }
    public boolean isForceDarkAllowed() { return mRenderNode.isForceDarkAllowed(); }
    public boolean hasOverlappingRendering() { return true; }
    public void setHasTransientState(boolean hasTransientState) {}
    public boolean hasTransientState() { return false; }
    public void setAnimationMatrix(Matrix matrix) { mRenderNode.setAnimationMatrix(matrix); invalidateViewProperty(); }
    public Matrix getAnimationMatrix() { return mRenderNode.getAnimationMatrix(); }
    public void setTransitionName(String transitionName) { mTransitionName = transitionName; }
    public String getTransitionName() { return mTransitionName; }
    public void setTransitionGroup(boolean isTransitionGroup) { mTransitionGroup = isTransitionGroup; }
    public boolean isTransitionGroup() { return mTransitionGroup; }
    public void setTransitionVisibility(int visibility) { mVisibility = visibility; }
    public ViewPropertyAnimator animate() { return new ViewPropertyAnimator(this); }
    public void clearAnimation() {}
    public long getDrawingTime() { return mAttachInfo != null ? mAttachInfo.drawingTime : 0; }

    // ---- measure and layout ----
    public final void measure(int widthMeasureSpec, int heightMeasureSpec) {
        boolean specChanged = widthMeasureSpec != mOldWidthSpec || heightMeasureSpec != mOldHeightSpec;
        if (mForceLayout || specChanged) {
            mMeasuredSet = false;
            onMeasure(widthMeasureSpec, heightMeasureSpec);
            if (!mMeasuredSet)
                throw new IllegalStateException(getClass().getName() + "#onMeasure() did not set the measured dimension by calling setMeasuredDimension()");
            mLayoutRequested = true;
        }
        mOldWidthSpec = widthMeasureSpec;
        mOldHeightSpec = heightMeasureSpec;
    }
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        setMeasuredDimension(getDefaultSize(getSuggestedMinimumWidth(), widthMeasureSpec),
            getDefaultSize(getSuggestedMinimumHeight(), heightMeasureSpec));
    }
    protected final void setMeasuredDimension(int measuredWidth, int measuredHeight) {
        mMeasuredWidth = measuredWidth;
        mMeasuredHeight = measuredHeight;
        mMeasuredSet = true;
    }
    public final int getMeasuredWidth() { return mMeasuredWidth & MEASURED_SIZE_MASK; }
    public final int getMeasuredHeight() { return mMeasuredHeight & MEASURED_SIZE_MASK; }
    public final int getMeasuredWidthAndState() { return mMeasuredWidth; }
    public final int getMeasuredHeightAndState() { return mMeasuredHeight; }
    public final int getMeasuredState() {
        return (mMeasuredWidth & MEASURED_STATE_MASK) | ((mMeasuredHeight >> MEASURED_HEIGHT_STATE_SHIFT) & (MEASURED_STATE_MASK >> MEASURED_HEIGHT_STATE_SHIFT));
    }
    public static int combineMeasuredStates(int curState, int newState) { return curState | newState; }
    public static int resolveSize(int size, int measureSpec) { return resolveSizeAndState(size, measureSpec, 0) & MEASURED_SIZE_MASK; }
    public static int resolveSizeAndState(int size, int measureSpec, int childMeasuredState) {
        int mode = MeasureSpec.getMode(measureSpec), specSize = MeasureSpec.getSize(measureSpec), result;
        if (mode == MeasureSpec.AT_MOST) result = specSize < size ? specSize | MEASURED_STATE_TOO_SMALL : size;
        else if (mode == MeasureSpec.EXACTLY) result = specSize;
        else result = size;
        return result | (childMeasuredState & MEASURED_STATE_MASK);
    }
    public static int getDefaultSize(int size, int measureSpec) {
        int mode = MeasureSpec.getMode(measureSpec);
        return mode == MeasureSpec.AT_MOST || mode == MeasureSpec.EXACTLY ? MeasureSpec.getSize(measureSpec) : size;
    }
    protected int getSuggestedMinimumWidth() { return mBackground == null ? mMinWidth : Math.max(mMinWidth, mBackground.getMinimumWidth()); }
    protected int getSuggestedMinimumHeight() { return mBackground == null ? mMinHeight : Math.max(mMinHeight, mBackground.getMinimumHeight()); }
    public int getMinimumWidth() { return mMinWidth; }
    public int getMinimumHeight() { return mMinHeight; }
    public void setMinimumWidth(int minWidth) { mMinWidth = minWidth; requestLayout(); }
    public void setMinimumHeight(int minHeight) { mMinHeight = minHeight; requestLayout(); }

    public void layout(int l, int t, int r, int b) {
        int ol = mLeft, ot = mTop, or = mRight, ob = mBottom;
        boolean changed = setFrame(l, t, r, b);
        if (changed || mLayoutRequested) {
            onLayout(changed, l, t, r, b);
            mLayoutRequested = false;
            for (OnLayoutChangeListener li : mLayoutListeners) li.onLayoutChange(this, l, t, r, b, ol, ot, or, ob);
        }
        mForceLayout = false;
        mLaidOut = true;
    }
    protected boolean setFrame(int left, int top, int right, int bottom) {
        if (mLeft == left && mRight == right && mTop == top && mBottom == bottom) return false;
        int oldWidth = mRight - mLeft, oldHeight = mBottom - mTop;
        invalidate();
        mLeft = left; mTop = top; mRight = right; mBottom = bottom;
        mRenderNode.setPosition(left, top, right, bottom);
        int newWidth = right - left, newHeight = bottom - top;
        if (newWidth != oldWidth || newHeight != oldHeight) {
            onSizeChanged(newWidth, newHeight, oldWidth, oldHeight);
            if (mBackground != null) mBackground.setBounds(0, 0, newWidth, newHeight);
            if (mForeground != null) mForeground.setBounds(0, 0, newWidth, newHeight);
            invalidateOutline();
        }
        invalidate();
        return true;
    }
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {}
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {}
    public boolean isLaidOut() { return mLaidOut; }
    public boolean isLayoutRequested() { return mForceLayout; }
    public void forceLayout() { mForceLayout = true; mLayoutRequested = true; }
    public void requestLayout() {
        mForceLayout = true;
        mLayoutRequested = true;
        if (mParent != null && !mParent.isLayoutRequested()) mParent.requestLayout();
    }
    public void addOnLayoutChangeListener(OnLayoutChangeListener l) { mLayoutListeners.add(l); }
    public void removeOnLayoutChangeListener(OnLayoutChangeListener l) { mLayoutListeners.remove(l); }
    public ViewGroup.LayoutParams getLayoutParams() { return mLayoutParams; }
    public void setLayoutParams(ViewGroup.LayoutParams params) {
        if (params == null) throw new NullPointerException("Layout parameters cannot be null");
        mLayoutParams = params;
        if (mParent instanceof ViewGroup g) g.onSetLayoutParams(this, params);
        requestLayout();
    }
    public int getBaseline() { return -1; }
    void setLayoutParamsQuietly(ViewGroup.LayoutParams params) { mLayoutParams = params; }

    // ---- drawing ----
    public void invalidate() {
        mDisplayListDirty = true;
        if (mParent != null) mParent.onDescendantInvalidated(this, this);
    }
    public void invalidate(Rect dirty) { invalidate(); }
    public void invalidate(int l, int t, int r, int b) { invalidate(); }
    void invalidateSelfAndParent() { invalidate(); if (mParent instanceof View p) p.invalidate(); }
    /** A property of the node changed: the parent's list draws it again, this one's stays. */
    void invalidateViewProperty() { if (mParent != null) mParent.onDescendantInvalidated(this, this); }
    boolean displayListDirty() { return mDisplayListDirty; }

    /** The view's RenderNode with its drawing recorded, again when it changed. */
    public RenderNode updateDisplayListIfDirty() {
        RenderNode node = mRenderNode;
        if (!mDisplayListDirty && node.hasDisplayList()) {
            if (this instanceof ViewGroup g) g.updateChildren();
            return node;
        }
        mDisplayListDirty = false;
        RecordingCanvasHolder.record(this, node);
        return node;
    }
    public RenderNode renderNode() { return mRenderNode; }

    /** The recording of a view's drawing into its node, as View.updateDisplayListIfDirty does. */
    static final class RecordingCanvasHolder {
        static void record(View v, RenderNode node) {
            android.graphics.RecordingCanvas c = node.beginRecording(v.getWidth(), v.getHeight());
            try {
                if (v.mScrollX != 0 || v.mScrollY != 0) c.translate(-v.mScrollX, -v.mScrollY);
                if (v.mWillNotDraw && v instanceof ViewGroup g && v.mBackground == null) g.dispatchDraw(c);
                else v.draw(c);
            } finally {
                node.endRecording();
            }
        }
    }

    public void draw(Canvas canvas) {
        if (mBackground != null) {
            mBackground.setBounds(0, 0, getWidth(), getHeight());
            mBackground.draw(canvas);
        }
        onDraw(canvas);
        dispatchDraw(canvas);
        onDrawForeground(canvas);
    }
    protected void onDraw(Canvas canvas) {}
    protected void dispatchDraw(Canvas canvas) {}
    public void onDrawForeground(Canvas canvas) {
        if (mForeground != null) {
            mForeground.setBounds(0, 0, getWidth(), getHeight());
            mForeground.draw(canvas);
        }
    }
    public void setDefaultFocusHighlightEnabled(boolean enabled) { mDefaultFocusHighlight = enabled; }
    public boolean getDefaultFocusHighlightEnabled() { return mDefaultFocusHighlight; }

    // ---- touch ----
    public void setOnClickListener(OnClickListener l) { if (!mClickable) mClickable = true; mOnClickListener = l; }
    public boolean hasOnClickListeners() { return mOnClickListener != null; }
    public void setOnLongClickListener(OnLongClickListener l) { if (!mLongClickable) mLongClickable = true; mOnLongClickListener = l; }
    public void setOnTouchListener(OnTouchListener l) { mOnTouchListener = l; }
    public void setOnKeyListener(OnKeyListener l) { mOnKeyListener = l; }
    public void setOnFocusChangeListener(OnFocusChangeListener l) { mOnFocusChangeListener = l; }
    public OnFocusChangeListener getOnFocusChangeListener() { return mOnFocusChangeListener; }
    public void setOnGenericMotionListener(OnGenericMotionListener l) { mOnGenericMotionListener = l; }
    public void setOnHoverListener(OnHoverListener l) { mOnHoverListener = l; }
    public void setOnDragListener(OnDragListener l) { mOnDragListener = l; }
    public void setOnCreateContextMenuListener(OnCreateContextMenuListener l) {}
    public void setOnContextClickListener(OnContextClickListener l) {}
    public boolean performClick() {
        if (mOnClickListener != null) {
            playSoundEffect(0);
            mOnClickListener.onClick(this);
            return true;
        }
        return false;
    }
    public boolean callOnClick() { if (mOnClickListener != null) { mOnClickListener.onClick(this); return true; } return false; }
    public boolean performLongClick() { return mOnLongClickListener != null && mOnLongClickListener.onLongClick(this); }
    public boolean performLongClick(float x, float y) { return performLongClick(); }
    public boolean performContextClick() { return false; }
    public boolean performContextClick(float x, float y) { return false; }

    public boolean dispatchTouchEvent(MotionEvent event) {
        if (mOnTouchListener != null && mEnabled && mOnTouchListener.onTouch(this, event)) return true;
        return onTouchEvent(event);
    }
    public boolean onTouchEvent(MotionEvent event) {
        boolean clickable = mClickable || mLongClickable;
        if (!mEnabled) return clickable;
        if (!clickable) return false;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: setPressed(true); break;
            case MotionEvent.ACTION_MOVE:
                if (event.getX() < 0 || event.getY() < 0 || event.getX() >= getWidth() || event.getY() >= getHeight()) setPressed(false);
                break;
            case MotionEvent.ACTION_UP:
                if (mPressed) { setPressed(false); performClick(); }
                break;
            case MotionEvent.ACTION_CANCEL: setPressed(false); break;
            default: break;
        }
        return true;
    }
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (mOnGenericMotionListener != null && mOnGenericMotionListener.onGenericMotion(this, event)) return true;
        return onGenericMotionEvent(event);
    }
    public boolean onGenericMotionEvent(MotionEvent event) { return false; }
    protected boolean dispatchHoverEvent(MotionEvent event) {
        if (mOnHoverListener != null && mOnHoverListener.onHover(this, event)) return true;
        return onHoverEvent(event);
    }
    public boolean onHoverEvent(MotionEvent event) { return false; }
    public boolean dispatchCapturedPointerEvent(MotionEvent event) { return false; }
    public boolean dispatchDragEvent(DragEvent event) { return mOnDragListener != null && mOnDragListener.onDrag(this, event) || onDragEvent(event); }
    public boolean onDragEvent(DragEvent event) { return false; }
    public boolean onFilterTouchEventForSecurity(MotionEvent event) { return true; }
    public boolean onInterceptTouchEvent(MotionEvent ev) { return false; }
    public void requestPointerCapture() {}
    public void releasePointerCapture() {}
    public boolean hasPointerCapture() { return false; }
    public void setPointerIcon(PointerIcon pointerIcon) { mPointerIcon = pointerIcon; }
    public PointerIcon getPointerIcon() { return mPointerIcon; }
    public PointerIcon onResolvePointerIcon(MotionEvent event, int pointerIndex) { return mPointerIcon; }
    public boolean isInTouchMode() { return true; }
    public void setSystemGestureExclusionRects(java.util.List<Rect> rects) {}
    public java.util.List<Rect> getSystemGestureExclusionRects() { return java.util.Collections.emptyList(); }
    public boolean startDragAndDrop(android.content.ClipData data, DragShadowBuilder shadowBuilder, Object myLocalState, int flags) { return false; }
    public void cancelDragAndDrop() {}

    public static class DragShadowBuilder {
        public DragShadowBuilder() {}
        public DragShadowBuilder(View view) {}
        public void onProvideShadowMetrics(android.graphics.Point outShadowSize, android.graphics.Point outShadowTouchPoint) {}
        public void onDrawShadow(Canvas canvas) {}
    }

    // ---- nested scrolling ----
    public void setNestedScrollingEnabled(boolean enabled) { mNestedScrolling = enabled; }
    public boolean isNestedScrollingEnabled() { return mNestedScrolling; }
    public boolean startNestedScroll(int axes) { return false; }
    public void stopNestedScroll() {}
    public boolean hasNestedScrollingParent() { return false; }
    public boolean dispatchNestedScroll(int dxConsumed, int dyConsumed, int dxUnconsumed, int dyUnconsumed, int[] offsetInWindow) { return false; }
    public boolean dispatchNestedPreScroll(int dx, int dy, int[] consumed, int[] offsetInWindow) { return false; }
    public boolean dispatchNestedFling(float velocityX, float velocityY, boolean consumed) { return false; }
    public boolean dispatchNestedPreFling(float velocityX, float velocityY) { return false; }

    // ---- keys ----
    public KeyEvent.DispatcherState getKeyDispatcherState() { return mAttachInfo != null ? mAttachInfo.root.keyDispatcherState() : null; }
    public boolean dispatchKeyEventPreIme(KeyEvent event) { return onKeyPreIme(event.getKeyCode(), event); }
    public boolean onKeyPreIme(int keyCode, KeyEvent event) { return false; }
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (mOnKeyListener != null && mEnabled && mOnKeyListener.onKey(this, event.getKeyCode(), event)) return true;
        return event.dispatch(this, getKeyDispatcherState(), this);
    }
    public boolean dispatchKeyShortcutEvent(KeyEvent event) { return onKeyShortcut(event.getKeyCode(), event); }
    public boolean onKeyShortcut(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if ((keyCode == KeyEvent.KEYCODE_ENTER || keyCode == KeyEvent.KEYCODE_DPAD_CENTER) && mClickable && mEnabled) { setPressed(true); return true; }
        return false;
    }
    @Override public boolean onKeyLongPress(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyUp(int keyCode, KeyEvent event) {
        if ((keyCode == KeyEvent.KEYCODE_ENTER || keyCode == KeyEvent.KEYCODE_DPAD_CENTER) && mPressed) { setPressed(false); return performClick(); }
        return false;
    }
    @Override public boolean onKeyMultiple(int keyCode, int count, KeyEvent event) { return false; }
    public void addOnUnhandledKeyEventListener(OnUnhandledKeyEventListener l) { mUnhandledKeyListeners.add(l); }
    public void removeOnUnhandledKeyEventListener(OnUnhandledKeyEventListener l) { mUnhandledKeyListeners.remove(l); }
    boolean dispatchUnhandledKey(KeyEvent event) {
        for (OnUnhandledKeyEventListener l : mUnhandledKeyListeners) if (l.onUnhandledKeyEvent(this, event)) return true;
        return false;
    }

    // ---- focus ----
    public boolean isFocusable() { return mFocusable; }
    public int getFocusable() { return mFocusable ? FOCUSABLE : NOT_FOCUSABLE; }
    public void setFocusable(boolean focusable) { mFocusable = focusable; if (!focusable) mFocusableInTouchMode = false; }
    public void setFocusable(int focusable) { setFocusable(focusable != NOT_FOCUSABLE); }
    public boolean isFocusableInTouchMode() { return mFocusableInTouchMode; }
    public void setFocusableInTouchMode(boolean focusableInTouchMode) { mFocusableInTouchMode = focusableInTouchMode; if (focusableInTouchMode) mFocusable = true; }
    public boolean isFocused() { return mFocused; }
    public boolean hasFocus() { return mFocused; }
    public boolean hasFocusable() { return mVisibility == VISIBLE && mFocusable; }
    public boolean hasExplicitFocusable() { return hasFocusable(); }
    public View findFocus() { return mFocused ? this : null; }
    public final boolean requestFocus() { return requestFocus(FOCUS_DOWN); }
    public final boolean requestFocus(int direction) { return requestFocus(direction, null); }
    public boolean requestFocus(int direction, Rect previouslyFocusedRect) {
        if (!mFocusable || mVisibility != VISIBLE || !mFocusableInTouchMode && isInTouchMode()) return false;
        if (mFocused) return true;
        if (mParent instanceof ViewGroup g) g.requestChildFocus(this, this);
        mFocused = true;
        onFocusChanged(true, direction, previouslyFocusedRect);
        refreshDrawableState();
        if (mAttachInfo != null) mAttachInfo.treeObserver.dispatchOnGlobalFocusChange(null, this);
        return true;
    }
    public final boolean requestFocusFromTouch() { return requestFocus(); }
    public void clearFocus() {
        if (!mFocused) return;
        unFocus();
        if (mParent instanceof ViewGroup g) g.clearChildFocus(this);
    }
    void unFocus() {
        if (!mFocused) return;
        mFocused = false;
        onFocusChanged(false, 0, null);
        refreshDrawableState();
    }
    protected void onFocusChanged(boolean gainFocus, int direction, Rect previouslyFocusedRect) {
        if (mOnFocusChangeListener != null) mOnFocusChangeListener.onFocusChange(this, gainFocus);
    }
    public View focusSearch(int direction) { return mParent != null ? mParent.focusSearch(this, direction) : null; }
    public void addFocusables(ArrayList<View> views, int direction) { addFocusables(views, direction, 0); }
    public void addFocusables(ArrayList<View> views, int direction, int focusableMode) { if (hasFocusable()) views.add(this); }
    public ArrayList<View> getFocusables(int direction) { ArrayList<View> r = new ArrayList<>(); addFocusables(r, direction); return r; }
    public boolean isFocusedByDefault() { return false; }
    public void setFocusedByDefault(boolean isFocusedByDefault) {}
    public boolean restoreDefaultFocus() { return requestFocus(); }
    public boolean onCheckIsTextEditor() { return false; }
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) { return null; }
    public boolean checkInputConnectionProxy(View view) { return false; }
    public void onProvideAutofillVirtualStructure(ViewStructure structure, int flags) {}

    // ---- insets ----
    public void setOnApplyWindowInsetsListener(OnApplyWindowInsetsListener listener) { mOnApplyWindowInsetsListener = listener; }
    public WindowInsets onApplyWindowInsets(WindowInsets insets) { return insets; }
    public WindowInsets dispatchApplyWindowInsets(WindowInsets insets) {
        if (mOnApplyWindowInsetsListener != null) return mOnApplyWindowInsetsListener.onApplyWindowInsets(this, insets);
        return onApplyWindowInsets(insets);
    }
    public WindowInsets getRootWindowInsets() { return mAttachInfo != null ? WindowInsets.CONSUMED : null; }
    public WindowInsets computeSystemWindowInsets(WindowInsets in, Rect outLocalInsets) {
        if (outLocalInsets != null) outLocalInsets.setEmpty();
        return in;
    }
    public void requestApplyInsets() { if (mAttachInfo != null) mAttachInfo.root.requestApplyInsets(); }
    @Deprecated public void requestFitSystemWindows() { requestApplyInsets(); }
    public void setFitsSystemWindows(boolean fitSystemWindows) {}
    public boolean getFitsSystemWindows() { return false; }
    public WindowInsetsController getWindowInsetsController() { return mAttachInfo != null ? mAttachInfo.root.insetsController() : null; }
    public void setWindowInsetsAnimationCallback(WindowInsetsAnimation.Callback callback) {}
    public void setSystemUiVisibility(int visibility) { mSystemUiVisibility = visibility; }
    public int getSystemUiVisibility() { return mSystemUiVisibility; }
    public int getWindowSystemUiVisibility() { return mSystemUiVisibility; }
    public void setOnSystemUiVisibilityChangeListener(OnSystemUiVisibilityChangeListener l) { mOnSystemUiVisibilityChangeListener = l; }
    public android.window.OnBackInvokedDispatcher findOnBackInvokedDispatcher() {
        return mAttachInfo != null ? mAttachInfo.root.onBackInvokedDispatcher() : null;
    }

    // ---- state saving ----
    public void saveHierarchyState(SparseArray<Parcelable> container) { dispatchSaveInstanceState(container); }
    protected void dispatchSaveInstanceState(SparseArray<Parcelable> container) {
        if (mId != NO_ID && mSaveEnabled) {
            Parcelable state = onSaveInstanceState();
            if (state != null) container.put(mId, state);
        }
    }
    public void restoreHierarchyState(SparseArray<Parcelable> container) { dispatchRestoreInstanceState(container); }
    protected void dispatchRestoreInstanceState(SparseArray<Parcelable> container) {
        if (mId != NO_ID) {
            Parcelable state = container.get(mId);
            if (state != null) onRestoreInstanceState(state);
        }
    }
    protected Parcelable onSaveInstanceState() { return AbsSavedState.EMPTY_STATE; }
    protected void onRestoreInstanceState(Parcelable state) {}

    // ---- accessibility, autofill, content capture: off ----
    public void setAccessibilityDelegate(AccessibilityDelegate delegate) { mAccessibilityDelegate = delegate; }
    public AccessibilityDelegate getAccessibilityDelegate() { return mAccessibilityDelegate; }
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        return mAccessibilityDelegate != null ? mAccessibilityDelegate.getAccessibilityNodeProvider(this) : null;
    }
    public AccessibilityNodeInfo createAccessibilityNodeInfo() {
        AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(this);
        onInitializeAccessibilityNodeInfo(info);
        return info;
    }
    public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
        if (mAccessibilityDelegate != null) mAccessibilityDelegate.onInitializeAccessibilityNodeInfo(this, info);
    }
    public void onInitializeAccessibilityEvent(AccessibilityEvent event) {}
    public void onPopulateAccessibilityEvent(AccessibilityEvent event) {}
    public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent event) { return false; }
    public void sendAccessibilityEvent(int eventType) {
        if (mAccessibilityDelegate != null) mAccessibilityDelegate.sendAccessibilityEvent(this, eventType);
    }
    void sendAccessibilityEventInternal(int eventType) {}
    public void sendAccessibilityEventUnchecked(AccessibilityEvent event) {}
    public boolean performAccessibilityAction(int action, Bundle arguments) {
        return mAccessibilityDelegate != null ? mAccessibilityDelegate.performAccessibilityAction(this, action, arguments)
            : performAccessibilityActionInternal(action, arguments);
    }
    boolean performAccessibilityActionInternal(int action, Bundle arguments) { return action == 0x10 && performClick(); }
    public CharSequence getAccessibilityClassName() { return View.class.getName(); }
    public int getImportantForAccessibility() { return mImportantForAccessibility; }
    public void setImportantForAccessibility(int mode) { mImportantForAccessibility = mode; }
    public boolean isImportantForAccessibility() { return mImportantForAccessibility != IMPORTANT_FOR_ACCESSIBILITY_NO; }
    public CharSequence getContentDescription() { return mContentDescription; }
    public void setContentDescription(CharSequence d) { mContentDescription = d; }
    public CharSequence getStateDescription() { return mStateDescription; }
    public void setStateDescription(CharSequence d) { mStateDescription = d; }
    public CharSequence getAccessibilityPaneTitle() { return mAccessibilityPaneTitle; }
    public void setAccessibilityPaneTitle(CharSequence t) { mAccessibilityPaneTitle = t; }
    public boolean isAccessibilityHeading() { return mAccessibilityHeading; }
    public void setAccessibilityHeading(boolean isHeading) { mAccessibilityHeading = isHeading; }
    public boolean isScreenReaderFocusable() { return mScreenReaderFocusable; }
    public void setScreenReaderFocusable(boolean screenReaderFocusable) { mScreenReaderFocusable = screenReaderFocusable; }
    public void setAccessibilityLiveRegion(int mode) {}
    public int getAccessibilityLiveRegion() { return ACCESSIBILITY_LIVE_REGION_NONE; }
    public void setAccessibilityTraversalBefore(int id) {}
    public void setAccessibilityTraversalAfter(int id) {}
    public void setLabelFor(int id) {}
    public boolean isAccessibilityFocused() { return false; }
    public void announceForAccessibility(CharSequence text) {}
    public void setTooltipText(CharSequence tooltipText) { mTooltipText = tooltipText; }
    public CharSequence getTooltipText() { return mTooltipText; }
    public AutofillId getAutofillId() {
        if (mAutofillId == null) mAutofillId = new AutofillId(mId != NO_ID ? mId : (int) mUniqueDrawingId);
        return mAutofillId;
    }
    public void setAutofillId(AutofillId id) { mAutofillId = id; }
    public int getImportantForAutofill() { return mImportantForAutofill; }
    public void setImportantForAutofill(int mode) { mImportantForAutofill = mode; }
    public boolean isImportantForAutofill() { return false; }
    public void setAutofillHints(String... autofillHints) {}
    public String[] getAutofillHints() { return null; }
    public int getAutofillType() { return AUTOFILL_TYPE_NONE; }
    public AutofillValue getAutofillValue() { return null; }
    public void autofill(AutofillValue value) {}
    public void autofill(SparseArray<AutofillValue> values) {}
    public void onProvideAutofillStructure(ViewStructure structure, int flags) {}
    public void onProvideStructure(ViewStructure structure) {}
    public void onProvideVirtualStructure(ViewStructure structure) {}
    public void dispatchProvideStructure(ViewStructure structure) { onProvideStructure(structure); }
    public void dispatchProvideAutofillStructure(ViewStructure structure, int flags) { onProvideAutofillStructure(structure, flags); }
    public int getImportantForContentCapture() { return mImportantForContentCapture; }
    public void setImportantForContentCapture(int mode) { mImportantForContentCapture = mode; }
    public boolean isImportantForContentCapture() { return false; }
    public ContentCaptureSession getContentCaptureSession() { return null; }
    public void setContentCaptureSession(ContentCaptureSession session) {}
    public void setViewTranslationCallback(android.view.translation.ViewTranslationCallback callback) {}
    public void clearViewTranslationCallback() {}
    public void onCreateViewTranslationRequests(int[] supportedFormats, java.util.function.Consumer<android.view.translation.ViewTranslationRequest> requestsCollector) {}
    public void onViewTranslationResponse(android.view.translation.ViewTranslationResponse response) {}
    public void setScrollCaptureHint(int hint) {}
    public void setScrollCaptureCallback(ScrollCaptureCallback callback) {}
    public void onScrollCaptureSearch(Rect localVisibleRect, android.graphics.Point windowOffset, java.util.function.Consumer<ScrollCaptureTarget> targets) {}
    public ActionMode startActionMode(ActionMode.Callback callback) { return null; }
    public ActionMode startActionMode(ActionMode.Callback callback, int type) { return null; }
    public void createContextMenu(ContextMenu menu) {}
    public boolean showContextMenu() { return false; }
    public boolean showContextMenu(float x, float y) { return false; }

    @Override
    public String toString() {
        return getClass().getName() + "{" + Integer.toHexString(System.identityHashCode(this)) + " " + mLeft + "," + mTop + "-" + mRight + "," + mBottom
            + (mId != NO_ID ? " #" + Integer.toHexString(mId) : "") + "}";
    }
}
