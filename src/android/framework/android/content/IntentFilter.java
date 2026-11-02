// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import java.util.ArrayList;
import java.util.Iterator;

/** The intents a receiver or a component takes: actions, categories and data. */
public class IntentFilter implements android.os.Parcelable {
    public static final int SYSTEM_HIGH_PRIORITY = 1000, SYSTEM_LOW_PRIORITY = -1000, MATCH_CATEGORY_EMPTY = 0x100000,
        MATCH_ADJUSTMENT_NORMAL = 0x8000, NO_MATCH_ACTION = -3;
    private final ArrayList<String> actions = new ArrayList<>(), categories = new ArrayList<>(), schemes = new ArrayList<>(), types = new ArrayList<>();
    private int priority;

    public static class MalformedMimeTypeException extends android.util.AndroidException {
        public MalformedMimeTypeException() {}
        public MalformedMimeTypeException(String name) { super(name); }
    }

    public IntentFilter() {}
    public IntentFilter(String action) { addAction(action); }
    public IntentFilter(String action, String dataType) throws MalformedMimeTypeException { addAction(action); addDataType(dataType); }
    public IntentFilter(IntentFilter o) { actions.addAll(o.actions); categories.addAll(o.categories); schemes.addAll(o.schemes); types.addAll(o.types); priority = o.priority; }

    public final void addAction(String action) { if (!actions.contains(action)) actions.add(action); }
    public final int countActions() { return actions.size(); }
    public final String getAction(int index) { return actions.get(index); }
    public final boolean hasAction(String action) { return action != null && actions.contains(action); }
    public final boolean matchAction(String action) { return hasAction(action); }
    public final Iterator<String> actionsIterator() { return actions.iterator(); }
    public final void addCategory(String category) { if (!categories.contains(category)) categories.add(category); }
    public final int countCategories() { return categories.size(); }
    public final String getCategory(int index) { return categories.get(index); }
    public final boolean hasCategory(String category) { return categories.contains(category); }
    public final void addDataScheme(String scheme) { schemes.add(scheme); }
    public final void addDataType(String type) throws MalformedMimeTypeException { types.add(type); }
    public final void addDataPath(String path, int type) {}
    public final void addDataAuthority(String host, String port) {}
    public final void setPriority(int priority) { this.priority = priority; }
    public final int getPriority() { return priority; }
    public final int match(String action, String type, String scheme, android.net.Uri data, java.util.Set<String> categories, String logTag) {
        return hasAction(action) ? MATCH_CATEGORY_EMPTY + MATCH_ADJUSTMENT_NORMAL : NO_MATCH_ACTION;
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
