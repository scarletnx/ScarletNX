// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view.animation;

import android.content.Context;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.util.AttributeSet;
import org.xmlpull.v1.XmlPullParser;

/** Animation helpers: the frame clock, and animations from anim XML resources. */
public class AnimationUtils {
    public static long currentAnimationTimeMillis() { return android.os.SystemClock.uptimeMillis(); }
    public static Interpolator loadInterpolator(android.content.Context context, int id) { return new AccelerateDecelerateInterpolator(); }

    /** An animation from an anim resource: set, alpha, scale, rotate and translate. */
    public static Animation loadAnimation(Context context, int id) throws Resources.NotFoundException {
        try (XmlResourceParser p = context.getResources().getAnimation(id)) {
            int e;
            while ((e = p.next()) != XmlPullParser.START_TAG && e != XmlPullParser.END_DOCUMENT) {}
            if (e != XmlPullParser.START_TAG) throw new Resources.NotFoundException("Animation resource ID #0x" + Integer.toHexString(id));
            return read(context, p);
        } catch (Exception ex) {
            if (ex instanceof Resources.NotFoundException n) throw n;
            Resources.NotFoundException n = new Resources.NotFoundException("Can't load animation resource ID #0x" + Integer.toHexString(id));
            n.initCause(ex);
            throw n;
        }
    }
    private static Animation read(Context context, XmlResourceParser p) throws Exception {
        String tag = p.getName();
        Animation a;
        switch (tag) {
            case "set": {
                AnimationSet set = new AnimationSet(context, p);
                int depth = p.getDepth(), e;
                while ((e = p.next()) != XmlPullParser.END_DOCUMENT && (e != XmlPullParser.END_TAG || p.getDepth() > depth))
                    if (e == XmlPullParser.START_TAG) set.addAnimation(read(context, p));
                return set;
            }
            case "alpha": a = new AlphaAnimation(context, p); break;
            case "scale": a = new ScaleAnimation(context, p); break;
            case "rotate": a = new RotateAnimation(context, p); break;
            case "translate": a = new TranslateAnimation(context, p); break;
            default: throw new RuntimeException("Unknown animation name: " + tag);
        }
        int depth = p.getDepth(), e;
        while ((e = p.next()) != XmlPullParser.END_DOCUMENT && (e != XmlPullParser.END_TAG || p.getDepth() > depth)) {}
        return a;
    }

    /** A size attribute as {type, value}: "50%" of the view, "50%p" of the parent, or a number of pixels. */
    static float[] size(AttributeSet attrs, String name) {
        String s = attrs != null ? attrs.getAttributeValue(Animation.ANDROID, name) : null;
        if (s == null) return new float[] {Animation.ABSOLUTE, 0};
        try {
            if (s.endsWith("%p")) return new float[] {Animation.RELATIVE_TO_PARENT, Float.parseFloat(s.substring(0, s.length() - 2)) / 100};
            if (s.endsWith("%")) return new float[] {Animation.RELATIVE_TO_SELF, Float.parseFloat(s.substring(0, s.length() - 1)) / 100};
            int end = 0;
            while (end < s.length() && "+-.0123456789eE".indexOf(s.charAt(end)) >= 0) ++end;
            float v = Float.parseFloat(s.substring(0, end));
            String unit = s.substring(end);
            if (unit.equals("dip") || unit.equals("dp") || unit.equals("sp")) v *= snx.android.Device.metrics().density;
            return new float[] {Animation.ABSOLUTE, v};
        } catch (RuntimeException e) {
            return new float[] {Animation.ABSOLUTE, 0};
        }
    }
}
