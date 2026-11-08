// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.xmlpull.v1;

import java.util.HashMap;
import java.util.Map;

/**
 * XmlPull's factory of parsers and serializers, as Android's: each parser is the framework's text parser
 * (android.util.Xml.newPullParser), with the features set on the factory, and each serializer its text serializer.
 */
public class XmlPullParserFactory {
    public static final String PROPERTY_NAME = "org.xmlpull.v1.XmlPullParserFactory";
    protected final Map<String, Boolean> features = new HashMap<>();

    protected XmlPullParserFactory() {}

    public static XmlPullParserFactory newInstance() throws XmlPullParserException { return new XmlPullParserFactory(); }
    public static XmlPullParserFactory newInstance(String classNames, Class context) throws XmlPullParserException {
        return newInstance();
    }

    public void setFeature(String name, boolean state) throws XmlPullParserException { features.put(name, state); }
    public boolean getFeature(String name) {
        Boolean value = features.get(name);
        return value != null && value;
    }
    public void setNamespaceAware(boolean awareness) { features.put(XmlPullParser.FEATURE_PROCESS_NAMESPACES, awareness); }
    public boolean isNamespaceAware() { return getFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES); }
    public void setValidating(boolean validating) { features.put(XmlPullParser.FEATURE_VALIDATION, validating); }
    public boolean isValidating() { return getFeature(XmlPullParser.FEATURE_VALIDATION); }

    public XmlPullParser newPullParser() throws XmlPullParserException {
        /* namespaces off unless asked, as Android's factory gives its parsers (Xml.newPullParser's are on) */
        XmlPullParser parser = android.util.Xml.newPullParser();
        parser.setFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES, isNamespaceAware());
        for (Map.Entry<String, Boolean> e : features.entrySet())
            if (e.getValue() && !e.getKey().equals(XmlPullParser.FEATURE_PROCESS_NAMESPACES)) parser.setFeature(e.getKey(), true);
        return parser;
    }
    public XmlSerializer newSerializer() throws XmlPullParserException { return android.util.Xml.newSerializer(); }
}
