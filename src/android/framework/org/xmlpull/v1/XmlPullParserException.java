// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow XmlPull's source of this class (public domain), as the Android Open Source Project carries it
package org.xmlpull.v1;

/** An error of an XmlPullParser, with the place in the document. */
public class XmlPullParserException extends Exception {
    protected Throwable detail;
    protected int row = -1;
    protected int column = -1;

    public XmlPullParserException(String s) {
        super(s);
    }

    public XmlPullParserException(String msg, XmlPullParser parser, Throwable chain) {
        super((msg == null ? "" : msg + " ") + (parser == null ? "" : "(position:" + parser.getPositionDescription()
            + ") ") + (chain == null ? "" : "caused by: " + chain), chain);
        if (parser != null) {
            row = parser.getLineNumber();
            column = parser.getColumnNumber();
        }
        detail = chain;
    }

    public Throwable getDetail() {
        return detail;
    }

    public int getLineNumber() {
        return row;
    }

    public int getColumnNumber() {
        return column;
    }
}
