// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.content;

import android.net.Uri;
import android.os.Bundle;
import android.os.Parcelable;
import java.io.Serializable;
import java.util.ArrayList;
import java.util.Set;
import java.util.TreeSet;

/** An operation to perform: an action on data, for a component or for whatever takes it. */
public class Intent implements Parcelable, Cloneable {
    public static final String ACTION_MAIN = "android.intent.action.MAIN", ACTION_VIEW = "android.intent.action.VIEW",
        ACTION_SEND = "android.intent.action.SEND", ACTION_SENDTO = "android.intent.action.SENDTO",
        ACTION_EDIT = "android.intent.action.EDIT", ACTION_PICK = "android.intent.action.PICK",
        ACTION_GET_CONTENT = "android.intent.action.GET_CONTENT", ACTION_OPEN_DOCUMENT = "android.intent.action.OPEN_DOCUMENT",
        ACTION_CREATE_DOCUMENT = "android.intent.action.CREATE_DOCUMENT", ACTION_CHOOSER = "android.intent.action.CHOOSER",
        ACTION_BOOT_COMPLETED = "android.intent.action.BOOT_COMPLETED", ACTION_TIME_CHANGED = "android.intent.action.TIME_SET",
        ACTION_TIMEZONE_CHANGED = "android.intent.action.TIMEZONE_CHANGED", ACTION_LOCALE_CHANGED = "android.intent.action.LOCALE_CHANGED",
        ACTION_CONFIGURATION_CHANGED = "android.intent.action.CONFIGURATION_CHANGED",
        ACTION_SCREEN_ON = "android.intent.action.SCREEN_ON", ACTION_SCREEN_OFF = "android.intent.action.SCREEN_OFF",
        ACTION_POWER_CONNECTED = "android.intent.action.ACTION_POWER_CONNECTED",
        ACTION_POWER_DISCONNECTED = "android.intent.action.ACTION_POWER_DISCONNECTED",
        ACTION_BATTERY_CHANGED = "android.intent.action.BATTERY_CHANGED", ACTION_BATTERY_LOW = "android.intent.action.BATTERY_LOW",
        ACTION_BATTERY_OKAY = "android.intent.action.BATTERY_OKAY", ACTION_DEVICE_STORAGE_LOW = "android.intent.action.DEVICE_STORAGE_LOW",
        ACTION_DEVICE_STORAGE_OK = "android.intent.action.DEVICE_STORAGE_OK", ACTION_PACKAGE_REPLACED = "android.intent.action.PACKAGE_REPLACED",
        ACTION_MY_PACKAGE_REPLACED = "android.intent.action.MY_PACKAGE_REPLACED", ACTION_CLOSE_SYSTEM_DIALOGS = "android.intent.action.CLOSE_SYSTEM_DIALOGS",
        ACTION_DIAL = "android.intent.action.DIAL", ACTION_INSERT = "android.intent.action.INSERT", ACTION_DELETE = "android.intent.action.DELETE",
        ACTION_APPLICATION_DETAILS_SETTINGS = "android.settings.APPLICATION_DETAILS_SETTINGS";
    public static final String CATEGORY_DEFAULT = "android.intent.category.DEFAULT", CATEGORY_LAUNCHER = "android.intent.category.LAUNCHER",
        CATEGORY_BROWSABLE = "android.intent.category.BROWSABLE", CATEGORY_OPENABLE = "android.intent.category.OPENABLE",
        CATEGORY_HOME = "android.intent.category.HOME", CATEGORY_LEANBACK_LAUNCHER = "android.intent.category.LEANBACK_LAUNCHER";
    public static final String EXTRA_TEXT = "android.intent.extra.TEXT", EXTRA_SUBJECT = "android.intent.extra.SUBJECT",
        EXTRA_STREAM = "android.intent.extra.STREAM", EXTRA_TITLE = "android.intent.extra.TITLE", EXTRA_INTENT = "android.intent.extra.INTENT",
        EXTRA_EMAIL = "android.intent.extra.EMAIL", EXTRA_MIME_TYPES = "android.intent.extra.MIME_TYPES",
        EXTRA_ALLOW_MULTIPLE = "android.intent.extra.ALLOW_MULTIPLE", EXTRA_LOCAL_ONLY = "android.intent.extra.LOCAL_ONLY",
        EXTRA_HTML_TEXT = "android.intent.extra.HTML_TEXT", EXTRA_CHOOSER_TARGETS = "android.intent.extra.CHOOSER_TARGETS",
        EXTRA_INITIAL_INTENTS = "android.intent.extra.INITIAL_INTENTS", EXTRA_PACKAGE_NAME = "android.intent.extra.PACKAGE_NAME";
    public static final int FLAG_GRANT_READ_URI_PERMISSION = 0x1, FLAG_GRANT_WRITE_URI_PERMISSION = 0x2,
        FLAG_FROM_BACKGROUND = 0x4, FLAG_DEBUG_LOG_RESOLUTION = 0x8, FLAG_EXCLUDE_STOPPED_PACKAGES = 0x10,
        FLAG_INCLUDE_STOPPED_PACKAGES = 0x20, FLAG_GRANT_PERSISTABLE_URI_PERMISSION = 0x40, FLAG_GRANT_PREFIX_URI_PERMISSION = 0x80,
        FLAG_ACTIVITY_NO_HISTORY = 0x40000000, FLAG_ACTIVITY_SINGLE_TOP = 0x20000000, FLAG_ACTIVITY_NEW_TASK = 0x10000000,
        FLAG_ACTIVITY_MULTIPLE_TASK = 0x08000000, FLAG_ACTIVITY_CLEAR_TOP = 0x04000000, FLAG_ACTIVITY_FORWARD_RESULT = 0x02000000,
        FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS = 0x00800000, FLAG_ACTIVITY_REORDER_TO_FRONT = 0x00020000,
        FLAG_ACTIVITY_NO_ANIMATION = 0x00010000, FLAG_ACTIVITY_CLEAR_TASK = 0x00008000, FLAG_ACTIVITY_NEW_DOCUMENT = 0x00080000,
        FLAG_RECEIVER_REGISTERED_ONLY = 0x40000000, FLAG_RECEIVER_FOREGROUND = 0x10000000;
    public static final int FILL_IN_ACTION = 1, FILL_IN_DATA = 2, FILL_IN_CATEGORIES = 4, FILL_IN_COMPONENT = 8,
        FILL_IN_PACKAGE = 16, URI_INTENT_SCHEME = 1;

    private String action, type, pkg, identifier;
    private Uri data;
    private ComponentName component;
    private int flags;
    private TreeSet<String> categories;
    private Bundle extras;
    private ClipData clipData;
    private Intent selector;
    private android.graphics.Rect sourceBounds;

    public static class ShortcutIconResource implements Parcelable {
        public String packageName;
        public String resourceName;

        public ShortcutIconResource() {}
        public static ShortcutIconResource fromContext(Context context, int resourceId) {
            ShortcutIconResource r = new ShortcutIconResource();
            r.packageName = context.getPackageName();
            r.resourceName = context.getResources().getResourceName(resourceId);
            return r;
        }
        @Override public String toString() { return resourceName; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeString(packageName); dest.writeString(resourceName); }
        public static final Parcelable.Creator<ShortcutIconResource> CREATOR = new Parcelable.Creator<>() {
            @Override public ShortcutIconResource createFromParcel(android.os.Parcel in) {
                ShortcutIconResource r = new ShortcutIconResource();
                r.packageName = in.readString();
                r.resourceName = in.readString();
                return r;
            }
            @Override public ShortcutIconResource[] newArray(int size) { return new ShortcutIconResource[size]; }
        };
    }

    public Intent() {}
    public Intent(Intent o) {
        action = o.action; type = o.type; pkg = o.pkg; data = o.data; component = o.component; flags = o.flags;
        identifier = o.identifier; clipData = o.clipData;
        if (o.sourceBounds != null) sourceBounds = new android.graphics.Rect(o.sourceBounds);
        if (o.categories != null) categories = new TreeSet<>(o.categories);
        if (o.extras != null) extras = new Bundle(o.extras);
    }
    public Intent(String action) { this.action = action; }
    public Intent(String action, Uri uri) { this.action = action; data = uri; }
    public Intent(Context packageContext, Class<?> cls) { component = new ComponentName(packageContext, cls); }
    public Intent(String action, Uri uri, Context packageContext, Class<?> cls) { this(action, uri); component = new ComponentName(packageContext, cls); }

    public static Intent createChooser(Intent target, CharSequence title) { Intent i = new Intent(ACTION_CHOOSER); i.putExtra(EXTRA_INTENT, target); return i; }
    public static Intent makeMainActivity(ComponentName mainActivity) { Intent i = new Intent(ACTION_MAIN); i.setComponent(mainActivity); i.addCategory(CATEGORY_LAUNCHER); return i; }
    public static Intent makeMainSelectorActivity(String selectorAction, String selectorCategory) {
        Intent i = new Intent(ACTION_MAIN);
        i.addCategory(CATEGORY_LAUNCHER);
        Intent selector = new Intent();
        selector.setAction(selectorAction);
        selector.addCategory(selectorCategory);
        i.setSelector(selector);
        return i;
    }
    public static Intent parseUri(String uri, int flags) { return new Intent(ACTION_VIEW, Uri.parse(uri)); }

    @Override public Object clone() { return new Intent(this); }
    public Intent cloneFilter() { Intent i = new Intent(); i.action = action; i.data = data; i.type = type; i.pkg = pkg; i.component = component; return i; }
    public String getAction() { return action; }
    public Intent setAction(String action) { this.action = action; return this; }
    public Uri getData() { return data; }
    public String getDataString() { return data != null ? data.toString() : null; }
    public String getScheme() { return data != null ? data.getScheme() : null; }
    public Intent setData(Uri data) { this.data = data; type = null; return this; }
    public Intent setDataAndNormalize(Uri data) { return setData(data); }
    public String getType() { return type; }
    public Intent setType(String type) { this.type = type; data = null; return this; }
    public Intent setTypeAndNormalize(String type) { return setType(type); }
    public Intent setDataAndType(Uri data, String type) { this.data = data; this.type = type; return this; }
    public String resolveType(Context context) { return type; }
    public String resolveType(ContentResolver resolver) { return type; }
    public String resolveTypeIfNeeded(ContentResolver resolver) { return component != null ? type : resolveType(resolver); }
    public android.graphics.Rect getSourceBounds() { return sourceBounds; }
    public void setSourceBounds(android.graphics.Rect r) { sourceBounds = r != null ? new android.graphics.Rect(r) : null; }
    public String getPackage() { return pkg; }
    public Intent setPackage(String packageName) { pkg = packageName; return this; }
    public ComponentName getComponent() { return component; }
    public Intent setComponent(ComponentName component) { this.component = component; return this; }
    public Intent setClassName(Context packageContext, String className) { component = new ComponentName(packageContext, className); return this; }
    public Intent setClassName(String packageName, String className) { component = new ComponentName(packageName, className); return this; }
    public Intent setClass(Context packageContext, Class<?> cls) { component = new ComponentName(packageContext, cls); return this; }
    public ComponentName resolveActivity(android.content.pm.PackageManager pm) { return component; }
    public int getFlags() { return flags; }
    public Intent setFlags(int flags) { this.flags = flags; return this; }
    public Intent addFlags(int flags) { this.flags |= flags; return this; }
    public void removeFlags(int flags) { this.flags &= ~flags; }
    public String getIdentifier() { return identifier; }
    public Intent setIdentifier(String identifier) { this.identifier = identifier; return this; }
    public boolean hasCategory(String category) { return categories != null && categories.contains(category); }
    public Set<String> getCategories() { return categories; }
    public Intent addCategory(String category) { if (categories == null) categories = new TreeSet<>(); categories.add(category); return this; }
    public void removeCategory(String category) { if (categories != null) categories.remove(category); }
    public Intent getSelector() { return selector; }
    public void setSelector(Intent selector) { this.selector = selector; }
    public ClipData getClipData() { return clipData; }
    public void setClipData(ClipData clip) { clipData = clip; }
    public void setExtrasClassLoader(ClassLoader loader) { if (extras != null) extras.setClassLoader(loader); }
    public boolean hasExtra(String name) { return extras != null && extras.containsKey(name); }
    public boolean hasFileDescriptors() { return false; }
    public Bundle getExtras() { return extras != null ? new Bundle(extras) : null; }
    private Bundle ex() { if (extras == null) extras = new Bundle(); return extras; }
    public Intent putExtras(Intent src) { if (src.extras != null) ex().putAll(src.extras); return this; }
    public Intent putExtras(Bundle extras) { ex().putAll(extras); return this; }
    public Intent replaceExtras(Intent src) { extras = src.extras != null ? new Bundle(src.extras) : null; return this; }
    public Intent replaceExtras(Bundle extras) { this.extras = extras != null ? new Bundle(extras) : null; return this; }
    public void removeExtra(String name) { if (extras != null) extras.remove(name); }
    public Intent putExtra(String name, boolean value) { ex().putBoolean(name, value); return this; }
    public Intent putExtra(String name, byte value) { ex().putByte(name, value); return this; }
    public Intent putExtra(String name, char value) { ex().putChar(name, value); return this; }
    public Intent putExtra(String name, short value) { ex().putShort(name, value); return this; }
    public Intent putExtra(String name, int value) { ex().putInt(name, value); return this; }
    public Intent putExtra(String name, long value) { ex().putLong(name, value); return this; }
    public Intent putExtra(String name, float value) { ex().putFloat(name, value); return this; }
    public Intent putExtra(String name, double value) { ex().putDouble(name, value); return this; }
    public Intent putExtra(String name, String value) { ex().putString(name, value); return this; }
    public Intent putExtra(String name, CharSequence value) { ex().putCharSequence(name, value); return this; }
    public Intent putExtra(String name, Parcelable value) { ex().putParcelable(name, value); return this; }
    public Intent putExtra(String name, Parcelable[] value) { ex().putParcelableArray(name, value); return this; }
    public Intent putExtra(String name, Serializable value) { ex().putSerializable(name, value); return this; }
    public Intent putExtra(String name, boolean[] value) { ex().putBooleanArray(name, value); return this; }
    public Intent putExtra(String name, byte[] value) { ex().putByteArray(name, value); return this; }
    public Intent putExtra(String name, int[] value) { ex().putIntArray(name, value); return this; }
    public Intent putExtra(String name, long[] value) { ex().putLongArray(name, value); return this; }
    public Intent putExtra(String name, String[] value) { ex().putStringArray(name, value); return this; }
    public Intent putExtra(String name, Bundle value) { ex().putBundle(name, value); return this; }
    public Intent putParcelableArrayListExtra(String name, ArrayList<? extends Parcelable> value) { ex().putParcelableArrayList(name, value); return this; }
    public Intent putIntegerArrayListExtra(String name, ArrayList<Integer> value) { ex().putIntegerArrayList(name, value); return this; }
    public Intent putStringArrayListExtra(String name, ArrayList<String> value) { ex().putStringArrayList(name, value); return this; }
    public boolean getBooleanExtra(String name, boolean def) { return extras == null ? def : extras.getBoolean(name, def); }
    public byte getByteExtra(String name, byte def) { return extras == null ? def : extras.getByte(name, def); }
    public short getShortExtra(String name, short def) { return extras == null ? def : extras.getShort(name, def); }
    public char getCharExtra(String name, char def) { return extras == null ? def : extras.getChar(name, def); }
    public int getIntExtra(String name, int def) { return extras == null ? def : extras.getInt(name, def); }
    public long getLongExtra(String name, long def) { return extras == null ? def : extras.getLong(name, def); }
    public float getFloatExtra(String name, float def) { return extras == null ? def : extras.getFloat(name, def); }
    public double getDoubleExtra(String name, double def) { return extras == null ? def : extras.getDouble(name, def); }
    public String getStringExtra(String name) { return extras == null ? null : extras.getString(name); }
    public CharSequence getCharSequenceExtra(String name) { return extras == null ? null : extras.getCharSequence(name); }
    public <T extends Parcelable> T getParcelableExtra(String name) { return extras == null ? null : extras.getParcelable(name); }
    public <T> T getParcelableExtra(String name, Class<T> clazz) { return extras == null ? null : extras.getParcelable(name, clazz); }
    public Parcelable[] getParcelableArrayExtra(String name) { return extras == null ? null : extras.getParcelableArray(name); }
    public <T extends Parcelable> ArrayList<T> getParcelableArrayListExtra(String name) { return extras == null ? null : extras.getParcelableArrayList(name); }
    public Serializable getSerializableExtra(String name) { return extras == null ? null : extras.getSerializable(name); }
    public ArrayList<Integer> getIntegerArrayListExtra(String name) { return extras == null ? null : extras.getIntegerArrayList(name); }
    public ArrayList<String> getStringArrayListExtra(String name) { return extras == null ? null : extras.getStringArrayList(name); }
    public boolean[] getBooleanArrayExtra(String name) { return extras == null ? null : extras.getBooleanArray(name); }
    public byte[] getByteArrayExtra(String name) { return extras == null ? null : extras.getByteArray(name); }
    public int[] getIntArrayExtra(String name) { return extras == null ? null : extras.getIntArray(name); }
    public long[] getLongArrayExtra(String name) { return extras == null ? null : extras.getLongArray(name); }
    public String[] getStringArrayExtra(String name) { return extras == null ? null : extras.getStringArray(name); }
    public Bundle getBundleExtra(String name) { return extras == null ? null : extras.getBundle(name); }
    public boolean filterEquals(Intent o) {
        return o != null && java.util.Objects.equals(action, o.action) && java.util.Objects.equals(data, o.data)
            && java.util.Objects.equals(type, o.type) && java.util.Objects.equals(pkg, o.pkg) && java.util.Objects.equals(component, o.component)
            && java.util.Objects.equals(categories, o.categories) && java.util.Objects.equals(identifier, o.identifier);
    }
    public int filterHashCode() { return java.util.Objects.hash(action, data, type, pkg, component, categories, identifier); }
    public int fillIn(Intent other, int flags) {
        if (action == null) action = other.action;
        if (data == null && type == null) { data = other.data; type = other.type; }
        if (component == null) component = other.component;
        if (other.extras != null) ex().putAll(other.extras);
        return 0;
    }
    public String toUri(int flags) { return (data != null ? data.toString() : "") + "#Intent;" + (action != null ? "action=" + action + ";" : "") + "end"; }
    @Override public String toString() {
        return "Intent { " + (action != null ? "act=" + action + " " : "") + (data != null ? "dat=" + data + " " : "")
            + (component != null ? "cmp=" + component.flattenToShortString() + " " : "") + (extras != null ? "(has extras) " : "") + "}";
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(new Intent(this)); }
    public static final Parcelable.Creator<Intent> CREATOR = new Parcelable.Creator<>() {
        @Override public Intent createFromParcel(android.os.Parcel in) { return (Intent) in.readValue(null); }
        @Override public Intent[] newArray(int size) { return new Intent[size]; }
    };
}
