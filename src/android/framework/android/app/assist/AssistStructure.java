// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.assist;

import android.view.autofill.AutofillId;
import android.view.autofill.AutofillValue;
import java.util.ArrayList;

/** A screen's structure, as an autofill or assist service reads it: its windows and their views */
public class AssistStructure implements android.os.Parcelable {
    private final ArrayList<WindowNode> windows = new ArrayList<>();

    public AssistStructure() {}
    public int getWindowNodeCount() { return windows.size(); }
    public WindowNode getWindowNodeAt(int index) { return windows.get(index); }
    public android.content.ComponentName getActivityComponent() { return null; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }

    public static class WindowNode {
        private final ViewNode root;
        private final CharSequence title;
        WindowNode(ViewNode root, CharSequence title) {
            this.root = root;
            this.title = title;
        }
        public ViewNode getRootViewNode() { return root; }
        public CharSequence getTitle() { return title; }
        public int getLeft() { return 0; }
        public int getTop() { return 0; }
        public int getWidth() { return 0; }
        public int getHeight() { return 0; }
        public int getDisplayId() { return 0; }
    }
    public static class ViewNode {
        final ArrayList<ViewNode> children = new ArrayList<>();
        AutofillId autofillId;
        AutofillValue autofillValue;
        String[] autofillHints;
        CharSequence[] autofillOptions;
        String className, hint, idEntry, idPackage, webDomain, webScheme;
        CharSequence text, contentDescription;
        int inputType, id = android.view.View.NO_ID, visibility;
        boolean focused;

        public ViewNode() {}
        public int getId() { return id; }
        public String getIdPackage() { return idPackage; }
        public String getIdType() { return null; }
        public String getIdEntry() { return idEntry; }
        public AutofillId getAutofillId() { return autofillId; }
        public int getAutofillType() { return 0; }
        public String[] getAutofillHints() { return autofillHints; }
        public AutofillValue getAutofillValue() { return autofillValue; }
        public CharSequence[] getAutofillOptions() { return autofillOptions; }
        public int getInputType() { return inputType; }
        public String getClassName() { return className; }
        public String getHint() { return hint; }
        public CharSequence getText() { return text; }
        public CharSequence getContentDescription() { return contentDescription; }
        public String getWebDomain() { return webDomain; }
        public String getWebScheme() { return webScheme; }
        public android.view.ViewStructure.HtmlInfo getHtmlInfo() { return null; }
        public int getVisibility() { return visibility; }
        public boolean isFocused() { return focused; }
        public int getChildCount() { return children.size(); }
        public ViewNode getChildAt(int index) { return children.get(index); }
    }
}
