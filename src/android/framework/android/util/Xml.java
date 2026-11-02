// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import org.xmlpull.v1.XmlPullParser;

/** XML helpers: a compiled parser is already its own AttributeSet. */
public class Xml {
    public static String FEATURE_RELAXED = "http://xmlpull.org/v1/doc/features.html#relaxed";

    public static AttributeSet asAttributeSet(XmlPullParser parser) {
        if (parser instanceof AttributeSet a) return a;
        throw new UnsupportedOperationException("snx: Xml.asAttributeSet of a text XML parser");
    }
}
