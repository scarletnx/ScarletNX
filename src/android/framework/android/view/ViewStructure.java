// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.os.Bundle;
import android.view.autofill.AutofillId;
import android.view.autofill.AutofillValue;

/** A view's structure for autofill and content capture: nothing reads it here. */
public abstract class ViewStructure {
    public abstract void setId(int id, String packageName, String typeName, String entryName);
    public abstract void setDimens(int left, int top, int scrollX, int scrollY, int width, int height);
    public abstract void setVisibility(int visibility);
    public abstract void setText(CharSequence text);
    public abstract void setText(CharSequence text, int selectionStart, int selectionEnd);
    public abstract void setClassName(String className);
    public abstract void setContentDescription(CharSequence contentDescription);
    public abstract void setHint(CharSequence hint);
    public abstract void setEnabled(boolean state);
    public abstract void setFocusable(boolean state);
    public abstract void setFocused(boolean state);
    public abstract void setClickable(boolean state);
    public abstract void setSelected(boolean state);
    public abstract void setCheckable(boolean state);
    public abstract void setChecked(boolean state);
    public abstract Bundle getExtras();
    public abstract boolean hasExtras();
    public abstract void setChildCount(int num);
    public abstract int addChildCount(int num);
    public abstract int getChildCount();
    public abstract ViewStructure newChild(int index);
    public abstract ViewStructure asyncNewChild(int index);
    public abstract AutofillId getAutofillId();
    public abstract void setAutofillId(AutofillId id);
    public abstract void setAutofillId(AutofillId parentId, int virtualId);
    public abstract void setAutofillType(int type);
    public abstract void setAutofillHints(String[] hint);
    public abstract void setAutofillValue(AutofillValue value);
    public abstract void setDataIsSensitive(boolean sensitive);
    public abstract void asyncCommit();
    public void setInputType(int inputType) {}
    public void setImportantForAutofill(int mode) {}
    public void setTextStyle(float size, int fgColor, int bgColor, int style) {}
    public void setTextIdEntry(String entryName) {}
    public void setTextLines(int[] charOffsets, int[] baselines) {}
    public void setAlpha(float alpha) {}
    public void setElevation(float elevation) {}
    public void setTransformation(android.graphics.Matrix matrix) {}
    public void setOpaque(boolean opaque) {}
    public void setActivated(boolean state) {}
    public void setLongClickable(boolean state) {}
    public void setContextClickable(boolean state) {}
    public void setAccessibilityFocused(boolean state) {}
    public void setWebDomain(String domain) {}
    public void setMinTextEms(int minEms) {}
    public void setMaxTextEms(int maxEms) {}
    public void setMaxTextLength(int maxLength) {}
    public void setLocaleList(android.os.LocaleList localeList) {}
    public void setAutofillOptions(CharSequence[] options) {}
    public void setHtmlInfo(Object htmlInfo) {}
    public CharSequence getText() { return null; }
    public CharSequence getHint() { return null; }
    /** a web page's element, as a page's structure describes it */
    public abstract static class HtmlInfo {
        public abstract String getTag();
        public abstract java.util.List<android.util.Pair<String, String>> getAttributes();
        public abstract static class Builder {
            public abstract Builder addAttribute(String name, String value);
            public abstract HtmlInfo build();
        }
    }
}
