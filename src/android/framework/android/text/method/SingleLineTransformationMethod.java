// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.method;

/** Shows line feeds as spaces, and carriage returns as nothing (a zero-width no-break space). */
public class SingleLineTransformationMethod extends ReplacementTransformationMethod {
    private static final char[] ORIGINAL = {'\n', '\r'}, REPLACEMENT = {' ', '﻿'};
    private static SingleLineTransformationMethod instance;

    public SingleLineTransformationMethod() {}
    public static SingleLineTransformationMethod getInstance() {
        if (instance == null) instance = new SingleLineTransformationMethod();
        return instance;
    }
    @Override protected char[] getOriginal() { return ORIGINAL; }
    @Override protected char[] getReplacement() { return REPLACEMENT; }
}
