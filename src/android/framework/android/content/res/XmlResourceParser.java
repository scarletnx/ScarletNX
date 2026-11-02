// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.res;

import android.util.AttributeSet;
import org.xmlpull.v1.XmlPullParser;

/** A parser of compiled XML from an APK, with its tags' attributes. */
public interface XmlResourceParser extends XmlPullParser, AttributeSet, AutoCloseable {
    @Override
    String getAttributeNamespace(int index);

    @Override
    void close();
}
