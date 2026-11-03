// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.textclassifier;

/** Gives the text classifier of the app: one that does nothing, until the app sets its own. */
public final class TextClassificationManager {
    private TextClassifier classifier;

    public TextClassificationManager(android.content.Context context) {}
    public TextClassifier getTextClassifier() { return classifier != null ? classifier : TextClassifier.NO_OP; }
    public void setTextClassifier(TextClassifier textClassifier) { classifier = textClassifier; }
}
