// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2017 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view.textclassifier;

/** Classifies text. No model runs here: a selection stays as asked, and text has no kind or links. */
public interface TextClassifier {
    String TYPE_UNKNOWN = "", TYPE_OTHER = "other", TYPE_EMAIL = "email", TYPE_PHONE = "phone", TYPE_ADDRESS = "address",
        TYPE_URL = "url", TYPE_DATE = "date", TYPE_DATE_TIME = "datetime", TYPE_FLIGHT_NUMBER = "flight";
    String WIDGET_TYPE_TEXTVIEW = "textview", WIDGET_TYPE_EDITTEXT = "edittext", WIDGET_TYPE_UNSELECTABLE_TEXTVIEW = "nosel-textview",
        WIDGET_TYPE_WEBVIEW = "webview", WIDGET_TYPE_EDIT_WEBVIEW = "edit-webview", WIDGET_TYPE_CUSTOM_TEXTVIEW = "customview",
        WIDGET_TYPE_CUSTOM_EDITTEXT = "customedit", WIDGET_TYPE_CUSTOM_UNSELECTABLE_TEXTVIEW = "nosel-customview",
        WIDGET_TYPE_NOTIFICATION = "notification", WIDGET_TYPE_CLIPBOARD = "clipboard", WIDGET_TYPE_UNKNOWN = "unknown";
    String EXTRA_FROM_TEXT_CLASSIFIER = "android.view.textclassifier.extra.FROM_TEXT_CLASSIFIER";
    TextClassifier NO_OP = new TextClassifier() {};

    default TextSelection suggestSelection(TextSelection.Request request) {
        return new TextSelection.Builder(request.getStartIndex(), request.getEndIndex()).build();
    }
    default TextSelection suggestSelection(CharSequence text, int selectionStartIndex, int selectionEndIndex, android.os.LocaleList defaultLocales) {
        return suggestSelection(new TextSelection.Request.Builder(text, selectionStartIndex, selectionEndIndex).setDefaultLocales(defaultLocales).build());
    }
    default TextClassification classifyText(CharSequence text, int startIndex, int endIndex, android.os.LocaleList defaultLocales) {
        return new TextClassification.Builder().setText(text.subSequence(startIndex, endIndex).toString()).build();
    }
    default int getMaxGenerateLinksTextLength() { return 100 * 1000; }
    default void destroy() {}
    default boolean isDestroyed() { return false; }
}
