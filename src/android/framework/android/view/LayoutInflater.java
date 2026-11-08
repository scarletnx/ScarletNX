// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view;

import android.content.Context;
import android.content.res.XmlResourceParser;
import android.util.AttributeSet;
import java.lang.reflect.Constructor;
import org.xmlpull.v1.XmlPullParser;

/** Views from compiled layout XML: each tag's class by its (Context, AttributeSet) constructor. */
public abstract class LayoutInflater {
    protected final Context mContext;
    private Factory factory;
    private Factory2 factory2;
    private Filter filter;

    public interface Factory { View onCreateView(String name, Context context, AttributeSet attrs); }
    public interface Factory2 extends Factory { View onCreateView(View parent, String name, Context context, AttributeSet attrs); }
    public interface Filter { boolean onLoadClass(Class clazz); }

    protected LayoutInflater(Context context) { mContext = context; }
    protected LayoutInflater(LayoutInflater original, Context newContext) {
        mContext = newContext;
        factory = original.factory;
        factory2 = original.factory2;
        filter = original.filter;
    }

    public static LayoutInflater from(Context context) {
        LayoutInflater i = (LayoutInflater) context.getSystemService(Context.LAYOUT_INFLATER_SERVICE);
        if (i == null) throw new AssertionError("LayoutInflater not found.");
        return i;
    }

    /** The runtime's inflater, as PhoneLayoutInflater is Android's. */
    public static LayoutInflater create(Context context) {
        return new LayoutInflater(context) {
            @Override public LayoutInflater cloneInContext(Context newContext) {
                LayoutInflater self = this;
                return new LayoutInflater(self, newContext) {
                    @Override public LayoutInflater cloneInContext(Context c) { return create(c); }
                };
            }
        };
    }

    public abstract LayoutInflater cloneInContext(Context newContext);
    public Context getContext() { return mContext; }
    public final Factory getFactory() { return factory; }
    public final Factory2 getFactory2() { return factory2; }
    public void setFactory(Factory factory) { this.factory = factory; }
    public void setFactory2(Factory2 factory) { factory2 = factory; this.factory = factory; }
    public Filter getFilter() { return filter; }
    public void setFilter(Filter filter) { this.filter = filter; }

    public View inflate(int resource, ViewGroup root) { return inflate(resource, root, root != null); }
    public View inflate(int resource, ViewGroup root, boolean attachToRoot) {
        try (XmlResourceParser p = mContext.getResources().getLayout(resource)) {
            return inflate(p, root, attachToRoot);
        }
    }
    public View inflate(XmlPullParser parser, ViewGroup root) { return inflate(parser, root, root != null); }

    public View inflate(XmlPullParser parser, ViewGroup root, boolean attachToRoot) {
        try {
            int type;
            while ((type = parser.next()) != XmlPullParser.START_TAG && type != XmlPullParser.END_DOCUMENT) {}
            if (type != XmlPullParser.START_TAG) throw new InflateException(parser.getPositionDescription() + ": No start tag found!");
            AttributeSet attrs = (AttributeSet) parser;
            if (parser.getName().equals("merge")) {
                /* a merge's children go into the root, which it needs */
                if (root == null || !attachToRoot)
                    throw new InflateException("<merge /> can be used only with a valid ViewGroup root and attachToRoot=true");
                inflateChildren(parser, root, attrs);
                return root;
            }
            View top = createView(root, parser.getName(), attrs);
            ViewGroup.LayoutParams params = root != null ? root.generateLayoutParams(attrs) : null;
            if (params != null && !attachToRoot) top.setLayoutParams(params);
            inflateChildren(parser, top, attrs);
            if (root != null && attachToRoot) {
                root.addView(top, params);
                return root;
            }
            return top;
        } catch (InflateException e) {
            throw e;
        } catch (Exception e) {
            throw new InflateException(parser.getPositionDescription() + ": " + e, e);
        }
    }

    private void inflateChildren(XmlPullParser parser, View parent, AttributeSet attrs) throws Exception {
        int depth = parser.getDepth(), type;
        while (((type = parser.next()) != XmlPullParser.END_TAG || parser.getDepth() > depth) && type != XmlPullParser.END_DOCUMENT) {
            if (type != XmlPullParser.START_TAG) continue;
            String name = parser.getName();
            if (name.equals("include")) {
                /* the layout it names, into the parent: a merge's children, or its root view */
                int layout = attrs.getAttributeResourceValue(null, "layout", 0);
                if (layout == 0) throw new InflateException("You must specify a valid layout reference in an <include /> tag");
                inflate(layout, (ViewGroup) parent, true);
                skip(parser);
                continue;
            }
            if (name.equals("requestFocus") || name.equals("tag") || name.equals("merge")) {
                skip(parser);
                continue;
            }
            View child = createView(parent, name, attrs);
            ViewGroup group = (ViewGroup) parent;
            ViewGroup.LayoutParams params = group.generateLayoutParams(attrs);
            inflateChildren(parser, child, attrs);
            group.addView(child, params);
        }
    }

    /** the rest of the element that the parser is at the start of */
    private static void skip(XmlPullParser parser) throws Exception {
        int depth = parser.getDepth(), type;
        while (((type = parser.next()) != XmlPullParser.END_TAG || parser.getDepth() > depth) && type != XmlPullParser.END_DOCUMENT) {}
    }

    private View createView(View parent, String name, AttributeSet attrs) throws Exception {
        View v = null;
        if (factory2 != null) v = factory2.onCreateView(parent, name, mContext, attrs);
        else if (factory != null) v = factory.onCreateView(name, mContext, attrs);
        if (v != null) return v;
        return name.indexOf('.') < 0 ? onCreateView(name, attrs) : createView(name, null, attrs);
    }

    protected View onCreateView(String name, AttributeSet attrs) throws ClassNotFoundException {
        for (String prefix : new String[] {"android.widget.", "android.view.", "android.webkit."}) {
            try {
                return createView(name, prefix, attrs);
            } catch (ClassNotFoundException e) {
                // the next prefix
            }
        }
        throw new ClassNotFoundException(name);
    }

    protected View onCreateView(View parent, String name, AttributeSet attrs) throws ClassNotFoundException { return onCreateView(name, attrs); }

    public final View createView(String name, String prefix, AttributeSet attrs) throws ClassNotFoundException {
        String full = prefix != null ? prefix + name : name;
        Class<?> c = Class.forName(full, false, mContext.getClassLoader());
        try {
            Constructor<?> k = c.getConstructor(Context.class, AttributeSet.class);
            return (View) k.newInstance(mContext, attrs);
        } catch (ReflectiveOperationException e) {
            throw new InflateException("Error inflating class " + full, e);
        }
    }

    public final View createView(Context viewContext, String name, String prefix, AttributeSet attrs) throws ClassNotFoundException {
        return createView(name, prefix, attrs);
    }
}
