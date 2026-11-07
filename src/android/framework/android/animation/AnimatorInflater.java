// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2010 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.animation;

import android.content.Context;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;

/** Makes animators from animator XML resources: set, objectAnimator, animator and propertyValuesHolder. */
public class AnimatorInflater {
    private static final String ANDROID = "http://schemas.android.com/apk/res/android";

    public AnimatorInflater() {}

    public static Animator loadAnimator(Context context, int id) throws Resources.NotFoundException {
        try (XmlResourceParser p = context.getResources().getAnimation(id)) {
            int e;
            while ((e = p.next()) != XmlPullParser.START_TAG && e != XmlPullParser.END_DOCUMENT) {}
            if (e != XmlPullParser.START_TAG) throw new Resources.NotFoundException("Animator resource ID #0x" + Integer.toHexString(id));
            return read(context, p);
        } catch (Exception ex) {
            if (ex instanceof Resources.NotFoundException n) throw n;
            Resources.NotFoundException n = new Resources.NotFoundException("Can't load animation resource ID #0x" + Integer.toHexString(id));
            n.initCause(ex);
            throw n;
        }
    }

    private static Animator read(Context context, XmlResourceParser p) throws Exception {
        String tag = p.getName();
        int depth = p.getDepth();
        ArrayList<Animator> children = new ArrayList<>();
        ArrayList<PropertyValuesHolder> holders = new ArrayList<>();
        Animator result;
        if (tag.equals("set")) {
            AnimatorSet s = new AnimatorSet();
            result = s;
        } else {
            ValueAnimator v = tag.equals("objectAnimator") ? new ObjectAnimator() : new ValueAnimator();
            v.setDuration(p.getAttributeIntValue(ANDROID, "duration", 300));
            v.setStartDelay(p.getAttributeIntValue(ANDROID, "startOffset", 0));
            v.setRepeatCount(p.getAttributeIntValue(ANDROID, "repeatCount", 0));
            v.setRepeatMode(p.getAttributeIntValue(ANDROID, "repeatMode", ValueAnimator.RESTART));
            int interpolator = p.getAttributeResourceValue(ANDROID, "interpolator", 0);
            if (interpolator != 0) v.setInterpolator(android.view.animation.AnimationUtils.loadInterpolator(context, interpolator));
            PropertyValuesHolder h = holder(p, p.getAttributeValue(ANDROID, "propertyName"));
            if (h != null) holders.add(h);
            result = v;
        }
        int e;
        while ((e = p.next()) != XmlPullParser.END_DOCUMENT && (e != XmlPullParser.END_TAG || p.getDepth() > depth)) {
            if (e != XmlPullParser.START_TAG) continue;
            if (p.getName().equals("propertyValuesHolder")) {
                PropertyValuesHolder h = holder(p, p.getAttributeValue(ANDROID, "propertyName"));
                if (h != null) holders.add(h);
            } else if (result instanceof AnimatorSet) {
                children.add(read(context, p));
            }
        }
        if (result instanceof AnimatorSet s) {
            Animator[] items = children.toArray(new Animator[0]);
            if (p.getAttributeIntValue(ANDROID, "ordering", 0) == 1) s.playSequentially(items);
            else s.playTogether(items);
        } else if (!holders.isEmpty()) {
            ((ValueAnimator) result).setValues(holders.toArray(new PropertyValuesHolder[0]));
            if (result instanceof ObjectAnimator o && o.getPropertyName() == null) o.setPropertyName(holders.get(0).getPropertyName());
        }
        return result;
    }

    /** The values of a tag's valueFrom and valueTo, or null without them. */
    private static PropertyValuesHolder holder(XmlResourceParser p, String name) {
        int type = p.getAttributeIntValue(ANDROID, "valueType", 0);
        String from = p.getAttributeValue(ANDROID, "valueFrom"), to = p.getAttributeValue(ANDROID, "valueTo");
        if (to == null && from == null) return null;
        boolean color = type == 3 || (to != null && to.startsWith("#")) || (from != null && from.startsWith("#"));
        name = name != null ? name : "";
        if (color || type == 1) {
            int[] v = from != null && to != null ? new int[] {(int) number(from, color), (int) number(to, color)}
                : new int[] {(int) number(to != null ? to : from, color)};
            PropertyValuesHolder h = PropertyValuesHolder.ofInt(name, v);
            if (color) h.setEvaluator(ArgbEvaluator.getInstance());
            return h;
        }
        float[] v = from != null && to != null ? new float[] {(float) number(from, false), (float) number(to, false)}
            : new float[] {(float) number(to != null ? to : from, false)};
        return PropertyValuesHolder.ofFloat(name, v);
    }

    /** A value as the binary XML gives it: a color "#aarrggbb", a number, or a dimension with its unit. */
    private static double number(String s, boolean color) {
        try {
            if (s.startsWith("#")) {
                long v = Long.parseLong(s.substring(1), 16);
                return (int) (s.length() <= 7 ? v | 0xff000000L : v);
            }
            int end = 0;
            while (end < s.length() && "+-.0123456789eE".indexOf(s.charAt(end)) >= 0) ++end;
            double v = Double.parseDouble(s.substring(0, end));
            String unit = s.substring(end);
            if (unit.equals("dip") || unit.equals("dp") || unit.equals("sp")) v *= snx.android.Device.metrics().density;
            return v;
        } catch (RuntimeException e) {
            return 0;
        }
    }
}
