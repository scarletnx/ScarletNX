// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.app.PendingIntent;
import android.content.Context;
import android.graphics.Bitmap;
import android.os.Parcel;
import android.os.Parcelable;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import java.lang.reflect.Method;
import java.util.ArrayList;

/**
 * A layout and a list of actions on its views, which apply() runs on a view that it inflates. A parcel holds the
 * package and the layout only: the actions do not cross a parcel, as no other process reads them here.
 */
public class RemoteViews implements Parcelable, LayoutInflater.Filter, Cloneable {
    private final String packageName;
    private final int layoutId;
    private ArrayList<Action> actions = new ArrayList<>();

    private interface Action { void apply(View root, Context context); }

    public static class ActionException extends RuntimeException {
        public ActionException(Exception ex) { super(ex); }
        public ActionException(String message) { super(message); }
    }

    public RemoteViews(String packageName, int layoutId) { this.packageName = packageName; this.layoutId = layoutId; }
    public RemoteViews(RemoteViews src) { packageName = src.packageName; layoutId = src.layoutId; actions = new ArrayList<>(src.actions); }
    public RemoteViews(Parcel parcel) { packageName = parcel.readString(); layoutId = parcel.readInt(); }

    public String getPackage() { return packageName; }
    public int getLayoutId() { return layoutId; }
    @Override @Deprecated public RemoteViews clone() { return new RemoteViews(this); }

    private void add(int viewId, java.util.function.Consumer<View> action) {
        actions.add((root, context) -> {
            View v = root.findViewById(viewId);
            if (v != null) action.accept(v);
        });
    }
    /** Calls a setter of the view by its name, as the reflection actions of Android do. */
    private void reflect(int viewId, String methodName, Class<?> type, Object value) {
        add(viewId, v -> {
            try {
                Method m = v.getClass().getMethod(methodName, type);
                m.invoke(v, value);
            } catch (ReflectiveOperationException e) {
                throw new ActionException(e);
            }
        });
    }

    public void addView(int viewId, RemoteViews nestedView) {
        actions.add((root, context) -> {
            View v = root.findViewById(viewId);
            if (v instanceof ViewGroup g) g.addView(nestedView.apply(context, g));
        });
    }
    public void removeAllViews(int viewId) { add(viewId, v -> { if (v instanceof ViewGroup g) g.removeAllViews(); }); }
    public void setViewVisibility(int viewId, int visibility) { add(viewId, v -> v.setVisibility(visibility)); }
    public void setViewPadding(int viewId, int left, int top, int right, int bottom) { add(viewId, v -> v.setPadding(left, top, right, bottom)); }
    public void setContentDescription(int viewId, CharSequence contentDescription) { add(viewId, v -> v.setContentDescription(contentDescription)); }
    public void setTextViewText(int viewId, CharSequence text) { add(viewId, v -> { if (v instanceof TextView t) t.setText(text); }); }
    public void setTextViewTextSize(int viewId, int units, float size) { add(viewId, v -> { if (v instanceof TextView t) t.setTextSize(units, size); }); }
    public void setTextColor(int viewId, int color) { add(viewId, v -> { if (v instanceof TextView t) t.setTextColor(color); }); }
    public void setImageViewResource(int viewId, int srcId) { add(viewId, v -> { if (v instanceof ImageView i) i.setImageResource(srcId); }); }
    public void setImageViewBitmap(int viewId, Bitmap bitmap) { add(viewId, v -> { if (v instanceof ImageView i) i.setImageBitmap(bitmap); }); }
    public void setImageViewUri(int viewId, android.net.Uri uri) { add(viewId, v -> { if (v instanceof ImageView i) i.setImageURI(uri); }); }
    public void setOnClickPendingIntent(int viewId, PendingIntent pendingIntent) {
        add(viewId, v -> v.setOnClickListener(pendingIntent == null ? null : clicked -> {
            try { pendingIntent.send(); } catch (PendingIntent.CanceledException e) { android.util.Log.w("RemoteViews", "Cannot send pending intent: ", e); }
        }));
    }
    public void setChronometerCountDown(int viewId, boolean isCountDown) { reflect(viewId, "setCountDown", boolean.class, isCountDown); }
    public void setBoolean(int viewId, String methodName, boolean value) { reflect(viewId, methodName, boolean.class, value); }
    public void setInt(int viewId, String methodName, int value) { reflect(viewId, methodName, int.class, value); }
    public void setLong(int viewId, String methodName, long value) { reflect(viewId, methodName, long.class, value); }
    public void setFloat(int viewId, String methodName, float value) { reflect(viewId, methodName, float.class, value); }
    public void setString(int viewId, String methodName, String value) { reflect(viewId, methodName, String.class, value); }
    public void setCharSequence(int viewId, String methodName, CharSequence value) { reflect(viewId, methodName, CharSequence.class, value); }

    /** Inflates the layout in the parent and runs the actions on it, in their order. */
    public View apply(Context context, ViewGroup parent) {
        View v = LayoutInflater.from(context).inflate(layoutId, parent, false);
        reapply(context, v);
        return v;
    }
    public void reapply(Context context, View v) { for (Action a : actions) a.apply(v, context); }
    @Override public boolean onLoadClass(Class clazz) { return true; }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeString(packageName); dest.writeInt(layoutId); }
    public static final Parcelable.Creator<RemoteViews> CREATOR = new Parcelable.Creator<RemoteViews>() {
        @Override public RemoteViews createFromParcel(Parcel parcel) { return new RemoteViews(parcel); }
        @Override public RemoteViews[] newArray(int size) { return new RemoteViews[size]; }
    };
}
