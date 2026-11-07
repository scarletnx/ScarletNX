// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.LocusId;
import android.graphics.drawable.Icon;
import android.os.PersistableBundle;
import android.os.UserHandle;
import java.util.Set;

/** A launcher shortcut of the app. */
public final class ShortcutInfo implements android.os.Parcelable {
    public static final int DISABLED_REASON_NOT_DISABLED = 0, DISABLED_REASON_BY_APP = 1;
    public static final android.os.Parcelable.Creator<ShortcutInfo> CREATOR = new android.os.Parcelable.Creator<>() {
        @Override public ShortcutInfo createFromParcel(android.os.Parcel source) { return (ShortcutInfo) source.readValue(null); }
        @Override public ShortcutInfo[] newArray(int size) { return new ShortcutInfo[size]; }
    };

    final String id, packageName;
    ComponentName activity;
    Set<String> categories;
    CharSequence shortLabel, longLabel, disabledMessage;
    Intent[] intents;
    PersistableBundle extras;
    LocusId locusId;
    Icon icon;
    int rank;
    boolean dynamic = true, pinned, enabled = true, longLived;
    long changed = System.currentTimeMillis();

    private ShortcutInfo(String id, String packageName) {
        this.id = id;
        this.packageName = packageName;
    }

    public String getId() { return id; }
    public String getPackage() { return packageName; }
    public ComponentName getActivity() { return activity; }
    public Set<String> getCategories() { return categories; }
    public CharSequence getShortLabel() { return shortLabel; }
    public CharSequence getLongLabel() { return longLabel; }
    public CharSequence getDisabledMessage() { return disabledMessage; }
    public int getDisabledReason() { return enabled ? DISABLED_REASON_NOT_DISABLED : DISABLED_REASON_BY_APP; }
    public Intent getIntent() { return intents != null && intents.length > 0 ? intents[intents.length - 1] : null; }
    public Intent[] getIntents() { return intents != null ? intents.clone() : null; }
    public PersistableBundle getExtras() { return extras; }
    public LocusId getLocusId() { return locusId; }
    public int getRank() { return rank; }
    public long getLastChangedTimestamp() { return changed; }
    public UserHandle getUserHandle() { return android.os.Process.myUserHandle(); }
    public boolean isDynamic() { return dynamic; }
    public boolean isPinned() { return pinned; }
    public boolean isCached() { return false; }
    public boolean isDeclaredInManifest() { return false; }
    public boolean isImmutable() { return false; }
    public boolean isEnabled() { return enabled; }
    public boolean hasKeyFieldsOnly() { return false; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }

    public static class Builder {
        private final ShortcutInfo info;

        public Builder(Context context, String id) {
            if (id == null || id.isEmpty()) throw new IllegalArgumentException("id cannot be empty");
            info = new ShortcutInfo(id, context.getPackageName());
        }
        public Builder setActivity(ComponentName activity) { info.activity = activity; return this; }
        public Builder setCategories(Set<String> categories) { info.categories = categories; return this; }
        public Builder setShortLabel(CharSequence label) { info.shortLabel = label; return this; }
        public Builder setLongLabel(CharSequence label) { info.longLabel = label; return this; }
        public Builder setDisabledMessage(CharSequence message) { info.disabledMessage = message; return this; }
        public Builder setIntent(Intent intent) { return setIntents(new Intent[] {intent}); }
        public Builder setIntents(Intent[] intents) { info.intents = intents.clone(); return this; }
        public Builder setExtras(PersistableBundle extras) { info.extras = extras; return this; }
        public Builder setLocusId(LocusId locusId) { info.locusId = locusId; return this; }
        public Builder setIcon(Icon icon) { info.icon = icon; return this; }
        public Builder setRank(int rank) { info.rank = rank; return this; }
        public Builder setLongLived(boolean longLived) { info.longLived = longLived; return this; }
        public Builder setPerson(android.app.Person person) { return this; }
        public Builder setPersons(android.app.Person[] persons) { return this; }
        public ShortcutInfo build() {
            if (info.shortLabel == null) throw new IllegalArgumentException("Shortcut must have a short label");
            return info;
        }
    }
}
